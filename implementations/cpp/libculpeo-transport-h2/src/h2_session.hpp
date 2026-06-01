#pragma once

// Internal H2Session — not part of the public API.
//
// H2Session wraps one nghttp2 session over a type-erased socket.
// It handles:
//   • The HTTP/2 read loop (mem_recv → callbacks → mem_send → async_write)
//   • Per-stream send queues (deferred DATA providers)
//   • Per-stream frame reassembly (CulpeoStream length-prefixed framing)
//   • Per-stream asio::experimental::channel for receive_frame() delivery
//
// All nghttp2 state mutations happen on a strand derived from the socket's
// executor.  External callers may call send_frame() / receive_frame() from
// any executor; the methods dispatch to the strand internally.
//
// CulpeoStream H2 framing (spec §C.3 / §C.4):
//   [4-byte BE length][1-byte type][culpeostream frame bytes]
//   where length = 1 (type byte) + len(culpeostream_frame)
//
// IMPORTANT: H2Session is always owned via shared_ptr; it uses
// enable_shared_from_this internally.

#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>
#include <asio/experimental/channel.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/strand.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <nghttp2/nghttp2.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace culpeo::h2 {

// ─── Frame type octets (spec §C.3) ────────────────────────────────────────────

inline constexpr uint8_t kTypeControl = 0x01;
inline constexpr uint8_t kTypeMedia   = 0x02;

// ─── H2 framing helpers ────────────────────────────────────────────────────────

/// Build a CulpeoStream H2 envelope: [4-byte BE length][1-byte type][payload].
/// length = 1 (type byte) + payload.size()
inline std::vector<uint8_t> encode_h2_envelope(uint8_t type_byte,
                                                std::span<const std::byte> payload)
{
    uint32_t len = 1u + static_cast<uint32_t>(payload.size());
    std::vector<uint8_t> out;
    out.reserve(5 + payload.size());
    out.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((len >>  8) & 0xFF));
    out.push_back(static_cast<uint8_t>( len        & 0xFF));
    out.push_back(type_byte);
    for (auto b : payload) out.push_back(static_cast<uint8_t>(b));
    return out;
}

// ─── Type-erased socket interface ─────────────────────────────────────────────

class ISocketStream {
public:
    virtual ~ISocketStream() = default;
    virtual asio::any_io_executor get_executor() = 0;
    virtual asio::awaitable<std::size_t> async_read_some(std::span<uint8_t> buf) = 0;
    virtual asio::awaitable<void>        async_write(std::span<const uint8_t> buf) = 0;
    virtual void close() = 0;
};

// Plain TCP socket wrapper
class TcpSocketStream final : public ISocketStream {
public:
    explicit TcpSocketStream(asio::ip::tcp::socket s)
        : sock_(std::move(s)) {}

    asio::any_io_executor get_executor() override {
        return sock_.get_executor();
    }

    asio::awaitable<std::size_t> async_read_some(std::span<uint8_t> buf) override {
        co_return co_await sock_.async_read_some(
            asio::buffer(buf.data(), buf.size()), asio::use_awaitable);
    }

    asio::awaitable<void> async_write(std::span<const uint8_t> buf) override {
        co_await asio::async_write(sock_,
            asio::buffer(buf.data(), buf.size()), asio::use_awaitable);
    }

    void close() override {
        asio::error_code ec;
        sock_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        sock_.close(ec);
    }

private:
    asio::ip::tcp::socket sock_;
};

// TLS socket wrapper
class TlsSocketStream final : public ISocketStream {
public:
    using SslSocket = asio::ssl::stream<asio::ip::tcp::socket>;

    explicit TlsSocketStream(SslSocket s)
        : sock_(std::move(s)) {}

    asio::any_io_executor get_executor() override {
        return sock_.get_executor();
    }

    asio::awaitable<std::size_t> async_read_some(std::span<uint8_t> buf) override {
        co_return co_await sock_.async_read_some(
            asio::buffer(buf.data(), buf.size()), asio::use_awaitable);
    }

    asio::awaitable<void> async_write(std::span<const uint8_t> buf) override {
        co_await asio::async_write(sock_,
            asio::buffer(buf.data(), buf.size()), asio::use_awaitable);
    }

    void close() override {
        asio::error_code ec;
        sock_.shutdown(ec);
    }

private:
    SslSocket sock_;
};

// ─── H2 receive reassembly buffer (public for testing) ───────────────────────

// Maximum CulpeoStream frame size we'll accept (1 MiB, per spec §C.4).
inline constexpr uint32_t kH2MaxFrameSize = 1u << 20;

/// Accumulates raw DATA bytes and parses out complete CulpeoStream frames.
/// Format: [4-byte BE length][1-byte type][N-byte payload]
/// where length = 1 (type) + N (payload bytes).
struct RecvState {
    std::vector<uint8_t> buf;

    /// Parse and remove all complete frames from the buffer.
    /// Returns {type_byte, payload} pairs.
    std::vector<std::pair<uint8_t, std::vector<std::byte>>> drain_frames();
};

// ─── H2Session ────────────────────────────────────────────────────────────────

class H2Session : public std::enable_shared_from_this<H2Session> {
public:
    enum class Mode { Client, Server };

    // Expose max-frame-size and RecvState type for unit tests
    static constexpr uint32_t kMaxFrameSize = kH2MaxFrameSize;

    // Expose RecvState as a public member type alias
    using RecvState = culpeo::h2::RecvState;

    // Frame channel capacity (per stream).
    static constexpr std::size_t kChannelCapacity = 256;

    explicit H2Session(std::unique_ptr<ISocketStream> socket, Mode mode);
    ~H2Session();

    H2Session(const H2Session&) = delete;
    H2Session& operator=(const H2Session&) = delete;

    // ── Startup ───────────────────────────────────────────────────────────────

    /// Run the HTTP/2 read/write loop.  co_spawn this as detached.
    /// Returns when the connection is closed (EOF or error).
    asio::awaitable<void> run();

    // ── Client helpers ────────────────────────────────────────────────────────

    /// Submit an HTTP/2 POST request.
    /// @return stream_id on success, -1 on error.
    asio::awaitable<int32_t> submit_request(const std::string& host,
                                             const std::string& path);

    // ── Server helpers ────────────────────────────────────────────────────────

    /// Register a callback invoked (on strand) when a new request stream
    /// is ready (HEADERS received and 200 response submitted).
    void set_new_stream_handler(
        std::function<asio::awaitable<void>(int32_t)> cb);

    // ── Frame I/O ─────────────────────────────────────────────────────────────

    /// Enqueue a CulpeoStream frame for sending on stream_id.
    /// The envelope (length prefix + type byte) is added here.
    /// Dispatches to strand; safe to call from any executor.
    asio::awaitable<void> send_frame(int32_t stream_id,
                                      uint8_t type_byte,
                                      std::span<const std::byte> payload);

    /// Wait for the next complete CulpeoStream frame on stream_id.
    /// Returns {type_byte, payload}.
    /// Throws asio::error::eof when the stream closes.
    asio::awaitable<std::pair<uint8_t, std::vector<std::byte>>>
    receive_frame(int32_t stream_id);

    /// Close stream_id (RST_STREAM).
    asio::awaitable<void> close_stream(int32_t stream_id, uint32_t error_code = 0);

    /// Close the entire connection (GOAWAY).
    asio::awaitable<void> close_session();

    [[nodiscard]] bool is_closed() const noexcept {
        return closed_.load(std::memory_order_acquire);
    }

private:
    // ─── Per-stream state ──────────────────────────────────────────────────────

    // Send queue for one stream
    struct SendBuf {
        std::vector<uint8_t> data;
        std::size_t offset{0};
    };
    struct SendQueue {
        std::deque<SendBuf> pending;
        bool closed{false};  // true → next drain returns EOF
    };

    // Per-stream channel for receive_frame() delivery
    using FrameChannel = asio::experimental::channel<
        asio::any_io_executor,
        void(asio::error_code, uint8_t, std::vector<std::byte>)>;

    struct StreamState {
        RecvState  recv;
        SendQueue  send;
        std::unique_ptr<FrameChannel> channel;
        bool       response_sent{false};  // server: 200 submitted
    };

    // ─── Fields ────────────────────────────────────────────────────────────────

    std::unique_ptr<ISocketStream> sock_;
    Mode mode_;
    nghttp2_session* ng_{nullptr};
    asio::strand<asio::any_io_executor> strand_;
    std::atomic<bool> closed_{false};

    std::map<int32_t, StreamState> streams_;

    // New-stream callback (server only)
    std::function<asio::awaitable<void>(int32_t)> new_stream_cb_;

    // ─── nghttp2 callbacks (static) ────────────────────────────────────────────

    // DATA provider read callback (called by nghttp2_session_mem_send)
    static ssize_t data_read_callback(nghttp2_session* session,
                                       int32_t stream_id,
                                       uint8_t* buf,
                                       std::size_t length,
                                       uint32_t* data_flags,
                                       nghttp2_data_source* source,
                                       void* user_data);

    // Called when a complete HTTP/2 frame has been received
    static int on_frame_recv_callback(nghttp2_session* session,
                                       const nghttp2_frame* frame,
                                       void* user_data);

    // Called for each DATA chunk received on a stream
    static int on_data_chunk_recv_callback(nghttp2_session* session,
                                            uint8_t flags,
                                            int32_t stream_id,
                                            const uint8_t* data,
                                            std::size_t len,
                                            void* user_data);

    // Called when a stream closes
    static int on_stream_close_callback(nghttp2_session* session,
                                         int32_t stream_id,
                                         uint32_t error_code,
                                         void* user_data);

    // Called at start of HEADERS frame (server: new request)
    static int on_begin_headers_callback(nghttp2_session* session,
                                          const nghttp2_frame* frame,
                                          void* user_data);

    // Called for each header within a HEADERS frame
    static int on_header_callback(nghttp2_session* session,
                                   const nghttp2_frame* frame,
                                   const uint8_t* name, std::size_t namelen,
                                   const uint8_t* value, std::size_t valuelen,
                                   uint8_t flags,
                                   void* user_data);

    // ─── Internal helpers (run on strand) ─────────────────────────────────────

    void init_nghttp2();

    // Flush all pending nghttp2 output to the wire.
    asio::awaitable<void> drain_to_wire();

    // Enqueue data on a stream's send queue and resume the data provider.
    // MUST be called on the strand.
    void enqueue_send(int32_t stream_id, std::vector<uint8_t> envelope);

    // Ensure StreamState exists for stream_id.
    StreamState& get_or_create_stream(int32_t stream_id);

    // Deliver parsed frames from recv buffer to the stream's channel.
    void dispatch_recv_frames(int32_t stream_id);

    // Server: submit 200 response with streaming DATA provider.
    void submit_server_response(int32_t stream_id);
};

} // namespace culpeo::h2
