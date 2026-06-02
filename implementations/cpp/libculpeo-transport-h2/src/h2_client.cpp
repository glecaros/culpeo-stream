// h2_client.cpp — H2Session core + CulpeoH2Client
//
// Contains:
//   • H2Session::RecvState::drain_frames()
//   • H2Session constructor/destructor and init_nghttp2()
//   • All nghttp2 static callbacks
//   • H2Session::run() read loop
//   • H2Session::drain_to_wire()
//   • H2Session::send_frame() / receive_frame() / close_stream() / close_session()
//   • CulpeoH2Client implementation

#include "h2_session.hpp"
#include "culpeo/h2_client.hpp"
#include "culpeo/h2_transport.hpp"

#include <asio/co_spawn.hpp>
#include <asio/connect.hpp>
#include <asio/detached.hpp>
#include <asio/dispatch.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace culpeo::h2 {

// ════════════════════════════════════════════════════════════════════════════
// RecvState::drain_frames
// ════════════════════════════════════════════════════════════════════════════

std::vector<std::pair<uint8_t, std::vector<std::byte>>>
RecvState::drain_frames()
{
    std::vector<std::pair<uint8_t, std::vector<std::byte>>> result;

    while (buf.size() >= 5) {
        // Parse 4-byte big-endian length
        uint32_t len = (static_cast<uint32_t>(buf[0]) << 24)
                     | (static_cast<uint32_t>(buf[1]) << 16)
                     | (static_cast<uint32_t>(buf[2]) <<  8)
                     |  static_cast<uint32_t>(buf[3]);

        if (len == 0 || len > H2Session::kMaxFrameSize) {
            // Malformed: discard the whole buffer to avoid infinite loops
            buf.clear();
            break;
        }

        // Full frame available?
        if (buf.size() < 4u + len) break;

        uint8_t type_byte = buf[4];

        // Payload = everything after the type byte
        std::vector<std::byte> payload(len - 1);
        for (std::size_t i = 0; i < len - 1; ++i)
            payload[i] = static_cast<std::byte>(buf[5 + i]);

        result.emplace_back(type_byte, std::move(payload));
        buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(4 + len));
    }

    return result;
}

// ════════════════════════════════════════════════════════════════════════════
// H2Session construction / destruction
// ════════════════════════════════════════════════════════════════════════════

H2Session::H2Session(std::unique_ptr<ISocketStream> socket, Mode mode,
                     uint32_t max_concurrent_streams)
    : sock_(std::move(socket))
    , mode_(mode)
    , strand_(asio::make_strand(sock_->get_executor()))
    , max_concurrent_streams_(max_concurrent_streams)
{}

H2Session::~H2Session()
{
    if (ng_) {
        nghttp2_session_del(ng_);
        ng_ = nullptr;
    }
}

void H2Session::init_nghttp2()
{
    nghttp2_session_callbacks* cbs = nullptr;
    nghttp2_session_callbacks_new(&cbs);

    nghttp2_session_callbacks_set_on_frame_recv_callback(
        cbs, &H2Session::on_frame_recv_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
        cbs, &H2Session::on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(
        cbs, &H2Session::on_stream_close_callback);
    nghttp2_session_callbacks_set_on_begin_headers_callback(
        cbs, &H2Session::on_begin_headers_callback);
    nghttp2_session_callbacks_set_on_header_callback(
        cbs, &H2Session::on_header_callback);

    if (mode_ == Mode::Client)
        nghttp2_session_client_new(&ng_, cbs, this);
    else
        nghttp2_session_server_new(&ng_, cbs, this);

    nghttp2_session_callbacks_del(cbs);

    // Submit initial SETTINGS.
    // SEC-025: advertise SETTINGS_MAX_CONCURRENT_STREAMS so well-behaved clients
    // do not exceed the limit. The guard in on_begin_headers_callback protects
    // against malicious clients that ignore SETTINGS.
    nghttp2_settings_entry settings[] = {
        { NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS,
          max_concurrent_streams_ },
        { NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 65535 },
    };
    nghttp2_submit_settings(ng_, NGHTTP2_FLAG_NONE, settings,
                            sizeof(settings) / sizeof(settings[0]));
}

// ════════════════════════════════════════════════════════════════════════════
// nghttp2 static callbacks
// ════════════════════════════════════════════════════════════════════════════

ssize_t H2Session::data_read_callback(nghttp2_session* /*session*/,
                                       int32_t stream_id,
                                       uint8_t* buf,
                                       std::size_t length,
                                       uint32_t* data_flags,
                                       nghttp2_data_source* /*source*/,
                                       void* user_data)
{
    auto* self = static_cast<H2Session*>(user_data);

    auto it = self->streams_.find(stream_id);
    if (it == self->streams_.end()) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }

    SendQueue& sq = it->second.send;

    if (sq.pending.empty()) {
        if (sq.closed) {
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
            return 0;
        }
        // No data ready — tell nghttp2 to wait until resume_data() is called
        return NGHTTP2_ERR_DEFERRED;
    }

    SendBuf& front = sq.pending.front();
    std::size_t avail = front.data.size() - front.offset;
    std::size_t n = std::min(avail, length);
    std::memcpy(buf, front.data.data() + front.offset, n);
    front.offset += n;

    if (front.offset >= front.data.size()) {
        sq.pending.pop_front();
    }

    // If more items remain, don't set EOF
    return static_cast<ssize_t>(n);
}

int H2Session::on_begin_headers_callback(nghttp2_session* session,
                                           const nghttp2_frame* frame,
                                           void* user_data)
{
    auto* self = static_cast<H2Session*>(user_data);

    // Server-side: new stream opened by client
    if (self->mode_ == Mode::Server
        && frame->hd.type == NGHTTP2_HEADERS
        && frame->headers.cat == NGHTTP2_HCAT_REQUEST)
    {
        int32_t sid = frame->hd.stream_id;

        // SEC-025: enforce SETTINGS_MAX_CONCURRENT_STREAMS on the server side.
        // This guards against malicious clients that send more HEADERS frames
        // than the advertised limit allows.
        if (self->streams_.size() >= static_cast<std::size_t>(self->max_concurrent_streams_)) {
            nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, sid,
                                      NGHTTP2_REFUSED_STREAM);
            self->refused_streams_.insert(sid);
            return 0;
        }

        self->get_or_create_stream(sid);
    }
    return 0;
}

int H2Session::on_header_callback(nghttp2_session* /*session*/,
                                    const nghttp2_frame* frame,
                                    const uint8_t* name, std::size_t namelen,
                                    const uint8_t* value, std::size_t valuelen,
                                    uint8_t /*flags*/,
                                    void* user_data)
{
    auto* self = static_cast<H2Session*>(user_data);

    // Only process HEADERS for server-side incoming requests.
    if (self->mode_ != Mode::Server
        || frame->hd.type != NGHTTP2_HEADERS
        || frame->headers.cat != NGHTTP2_HCAT_REQUEST)
    {
        return 0;
    }

    int32_t sid = frame->hd.stream_id;

    // Skip refused streams (SEC-025).
    if (self->refused_streams_.count(sid)) return 0;

    auto it = self->streams_.find(sid);
    if (it == self->streams_.end()) return 0;

    // SEC-028: capture security-relevant request headers.
    std::string_view k(reinterpret_cast<const char*>(name),  namelen);
    std::string_view v(reinterpret_cast<const char*>(value), valuelen);

    if (k == "authorization")         it->second.authorization  = std::string(v);
    else if (k == "content-type")     it->second.content_type   = std::string(v);
    else if (k == "culpeostream-version") it->second.proto_version = std::string(v);

    return 0;
}

int H2Session::on_frame_recv_callback(nghttp2_session* session,
                                       const nghttp2_frame* frame,
                                       void* user_data)
{
    auto* self = static_cast<H2Session*>(user_data);
    int32_t sid = frame->hd.stream_id;

    if (frame->hd.type == NGHTTP2_HEADERS) {
        // Server: complete request HEADERS → validate and submit 200 response
        if (self->mode_ == Mode::Server
            && frame->headers.cat == NGHTTP2_HCAT_REQUEST)
        {
            // SEC-025: skip refused streams.
            if (self->refused_streams_.count(sid)) return 0;

            // SEC-028: validate Content-Type.
            // Accept "application/culpeostream", "application/octet-stream",
            // or empty (for backwards compatibility in cleartext tests).
            auto it = self->streams_.find(sid);
            if (it != self->streams_.end()) {
                const auto& ct = it->second.content_type;
                if (!ct.empty()
                    && ct != "application/culpeostream"
                    && ct != "application/octet-stream")
                {
                    // Unsupported media type — send RST_STREAM and refuse.
                    nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, sid,
                                              NGHTTP2_INTERNAL_ERROR);
                    self->refused_streams_.insert(sid);
                    return 0;
                }
            }

            self->submit_server_response(sid);

            // Spawn handler coroutine if registered
            if (self->new_stream_cb_) {
                auto cb = self->new_stream_cb_;
                auto self_ptr = self->shared_from_this();
                asio::co_spawn(self->strand_,
                    [cb, sid, self_ptr]() -> asio::awaitable<void> {
                        co_await cb(sid);
                    },
                    asio::detached);
            }
        }

        // Client: received 200 response HEADERS — stream is ready
        // (no special action needed; we can start sending DATA immediately)
    }

    // SETTINGS ACK or WINDOW_UPDATE — nghttp2 handles internally
    (void)session;
    return 0;
}

int H2Session::on_data_chunk_recv_callback(nghttp2_session* /*session*/,
                                             uint8_t /*flags*/,
                                             int32_t stream_id,
                                             const uint8_t* data,
                                             std::size_t len,
                                             void* user_data)
{
    auto* self = static_cast<H2Session*>(user_data);

    // SEC-025: skip data on refused streams.
    if (self->refused_streams_.count(stream_id)) return 0;

    auto& st = self->get_or_create_stream(stream_id);
    st.recv.buf.insert(st.recv.buf.end(), data, data + len);
    self->dispatch_recv_frames(stream_id);
    return 0;
}

int H2Session::on_stream_close_callback(nghttp2_session* /*session*/,
                                          int32_t stream_id,
                                          uint32_t /*error_code*/,
                                          void* user_data)
{
    auto* self = static_cast<H2Session*>(user_data);

    // SEC-025: clean up refused-stream bookkeeping.
    self->refused_streams_.erase(stream_id);

    auto it = self->streams_.find(stream_id);
    if (it == self->streams_.end()) return 0;

    // Send EOF through the receive channel
    if (it->second.channel) {
        // Try to send EOF; ignore if channel full (stream is being torn down)
        it->second.channel->try_send(
            asio::error::make_error_code(asio::error::eof), 0u,
            std::vector<std::byte>{});
    }

    self->streams_.erase(it);
    return 0;
}

// ════════════════════════════════════════════════════════════════════════════
// Internal helpers
// ════════════════════════════════════════════════════════════════════════════

H2Session::StreamState& H2Session::get_or_create_stream(int32_t stream_id)
{
    auto it = streams_.find(stream_id);
    if (it != streams_.end()) return it->second;

    StreamState& st = streams_[stream_id];
    st.channel = std::make_unique<FrameChannel>(strand_, kChannelCapacity);
    return st;
}

std::string H2Session::get_stream_header(int32_t stream_id,
                                          std::string_view name) const
{
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) return {};

    const auto& st = it->second;
    if (name == "authorization")          return st.authorization;
    if (name == "content-type")           return st.content_type;
    if (name == "culpeostream-version")   return st.proto_version;
    return {};
}

void H2Session::dispatch_recv_frames(int32_t stream_id)
{
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) return;

    auto frames = it->second.recv.drain_frames();
    for (auto& [type, payload] : frames) {
        if (it->second.channel) {
            it->second.channel->try_send(
                asio::error_code{}, type, std::move(payload));
        }
    }
}

void H2Session::submit_server_response(int32_t stream_id)
{
    auto& st = get_or_create_stream(stream_id);
    if (st.response_sent) return;
    st.response_sent = true;

    // Set up a streaming data provider for the response body
    nghttp2_data_provider dp{};
    dp.source.ptr = this;
    dp.read_callback = &H2Session::data_read_callback;

    std::array<nghttp2_nv, 2> hdrs{{
        {(uint8_t*)":status",      (uint8_t*)"200",
         7, 3, NGHTTP2_NV_FLAG_NONE},
        {(uint8_t*)"content-type", (uint8_t*)"application/culpeostream",
         12, 24, NGHTTP2_NV_FLAG_NONE},
    }};

    nghttp2_submit_response(ng_, stream_id, hdrs.data(), hdrs.size(), &dp);
}

void H2Session::set_new_stream_handler(
    std::function<asio::awaitable<void>(int32_t)> cb)
{
    new_stream_cb_ = std::move(cb);
}

// ════════════════════════════════════════════════════════════════════════════
// drain_to_wire — flush nghttp2 output to socket
// ════════════════════════════════════════════════════════════════════════════

asio::awaitable<void> H2Session::drain_to_wire()
{
    // Accumulate all pending bytes from nghttp2
    std::vector<uint8_t> out_buf;
    const uint8_t* ptr = nullptr;
    ssize_t n = 0;
    while ((n = nghttp2_session_mem_send(ng_, &ptr)) > 0) {
        out_buf.insert(out_buf.end(), ptr, ptr + n);
    }

    if (!out_buf.empty()) {
        co_await sock_->async_write(out_buf);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// H2Session::run — main read/write loop
// ════════════════════════════════════════════════════════════════════════════

asio::awaitable<void> H2Session::run()
{
    // Ensure we're on the strand
    co_await asio::dispatch(strand_, asio::use_awaitable);

    init_nghttp2();

    // For client mode: nghttp2 already queued the client preface + SETTINGS
    // via nghttp2_submit_settings in init_nghttp2; drain_to_wire flushes it.
    co_await drain_to_wire();

    std::array<uint8_t, 16384> read_buf{};

    while (!closed_.load(std::memory_order_acquire)) {
        std::size_t n = 0;
        try {
            n = co_await sock_->async_read_some(read_buf);
        } catch (const asio::system_error& e) {
            if (e.code() == asio::error::eof
                || e.code() == asio::error::connection_reset) {
                break;
            }
            break;
        }

        if (n == 0) break;

        ssize_t rv = nghttp2_session_mem_recv(
            ng_, read_buf.data(), n);
        if (rv < 0) break;

        co_await drain_to_wire();

        // Check if nghttp2 wants us to close
        if (!nghttp2_session_want_read(ng_) && !nghttp2_session_want_write(ng_)) {
            break;
        }
    }

    closed_.store(true, std::memory_order_release);
    sock_->close();

    // Close all open stream channels with EOF
    for (auto& [sid, st] : streams_) {
        if (st.channel) {
            st.channel->try_send(
                asio::error::make_error_code(asio::error::eof), 0u,
                std::vector<std::byte>{});
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// send_frame / receive_frame / close_stream / close_session
// ════════════════════════════════════════════════════════════════════════════

asio::awaitable<void> H2Session::send_frame(int32_t stream_id,
                                              uint8_t type_byte,
                                              std::span<const std::byte> payload)
{
    // Dispatch to strand so all nghttp2 access is serialized
    co_await asio::dispatch(strand_, asio::use_awaitable);

    if (closed_.load(std::memory_order_acquire))
        throw asio::system_error(asio::error::broken_pipe);

    // SEC-033: reject payloads that would overflow the 4-byte length prefix or
    // exceed the per-frame size cap. The type byte occupies 1 byte of the
    // length field, so the maximum payload size is kMaxFrameSize - 1.
    if (payload.size() >= static_cast<std::size_t>(kMaxFrameSize)) {
        throw std::invalid_argument("send_frame: payload exceeds kMaxFrameSize");
    }

    auto envelope = encode_h2_envelope(type_byte, payload);
    enqueue_send(stream_id, std::move(envelope));
    co_await drain_to_wire();
}

void H2Session::enqueue_send(int32_t stream_id, std::vector<uint8_t> envelope)
{
    auto& st = get_or_create_stream(stream_id);
    bool was_empty = st.send.pending.empty();
    st.send.pending.push_back(SendBuf{std::move(envelope), 0});

    if (was_empty) {
        // Resume the data provider so nghttp2 knows to call read_callback again
        nghttp2_session_resume_data(ng_, stream_id);
    }
}

asio::awaitable<std::pair<uint8_t, std::vector<std::byte>>>
H2Session::receive_frame(int32_t stream_id)
{
    // Ensure stream state exists (co_await may switch executor)
    co_await asio::dispatch(strand_, asio::use_awaitable);

    auto& st = get_or_create_stream(stream_id);
    FrameChannel* ch = st.channel.get();

    // async_receive with use_awaitable: throws asio::system_error on error_code.
    // When the stream closes we send error_code{eof} which causes a throw here.
    // The strand is released during the co_await so the read loop can run.
    auto [type, payload] = co_await ch->async_receive(asio::use_awaitable);
    co_return std::make_pair(type, std::move(payload));
}

asio::awaitable<void> H2Session::close_stream(int32_t stream_id,
                                                uint32_t error_code)
{
    co_await asio::dispatch(strand_, asio::use_awaitable);
    if (closed_.load(std::memory_order_acquire)) co_return;

    uint32_t ng_err = (error_code == 0) ? NGHTTP2_NO_ERROR : error_code;

    auto it = streams_.find(stream_id);
    if (it != streams_.end()) {
        it->second.send.closed = true;
        // resume_data() wakes up a deferred data provider so it can return
        // NGHTTP2_DATA_FLAG_EOF, producing a clean END_STREAM DATA frame.
        int rv = nghttp2_session_resume_data(ng_, stream_id);
        if (rv != 0) {
            // Data provider was not deferred (stream never entered DATA phase,
            // or SETTINGS not yet ACKed).  Fall back to RST_STREAM.
            nghttp2_submit_rst_stream(ng_, NGHTTP2_FLAG_NONE, stream_id, ng_err);
        }
    } else {
        // Stream is not tracked in streams_ (e.g. client-side stream that never
        // called receive_frame / never sent DATA).  We still need to tell the
        // peer the stream is gone; submit RST_STREAM unconditionally.
        nghttp2_submit_rst_stream(ng_, NGHTTP2_FLAG_NONE, stream_id, ng_err);
    }
    co_await drain_to_wire();
}

asio::awaitable<void> H2Session::close_session()
{
    co_await asio::dispatch(strand_, asio::use_awaitable);
    if (closed_.load(std::memory_order_acquire)) co_return;

    nghttp2_submit_goaway(ng_, NGHTTP2_FLAG_NONE, 0, NGHTTP2_NO_ERROR,
                          nullptr, 0);
    co_await drain_to_wire();
    closed_.store(true, std::memory_order_release);
    sock_->close();
}

asio::awaitable<int32_t> H2Session::submit_request(const std::string& host,
                                                     const std::string& path)
{
    co_await asio::dispatch(strand_, asio::use_awaitable);

    // Build headers per spec §C.2
    std::array<nghttp2_nv, 6> hdrs{{
        {(uint8_t*)":method",             (uint8_t*)"POST",
         7, 4, NGHTTP2_NV_FLAG_NONE},
        {(uint8_t*)":scheme",             (uint8_t*)"https",
         7, 5, NGHTTP2_NV_FLAG_NONE},
        {(uint8_t*)":path",               (uint8_t*)path.c_str(),
         5, static_cast<uint32_t>(path.size()), NGHTTP2_NV_FLAG_NONE},
        {(uint8_t*)":authority",          (uint8_t*)host.c_str(),
         10, static_cast<uint32_t>(host.size()), NGHTTP2_NV_FLAG_NONE},
        {(uint8_t*)"content-type",        (uint8_t*)"application/culpeostream",
         12, 24, NGHTTP2_NV_FLAG_NONE},
        {(uint8_t*)"culpeostream-version",(uint8_t*)"1.0",
         20, 3, NGHTTP2_NV_FLAG_NONE},
    }};

    // Streaming request body (DATA frames follow)
    nghttp2_data_provider dp{};
    dp.source.ptr = this;
    dp.read_callback = &H2Session::data_read_callback;

    int32_t sid = nghttp2_submit_request(
        ng_, nullptr, hdrs.data(), hdrs.size(), &dp, nullptr);

    if (sid < 0) {
        co_return -1;
    }

    get_or_create_stream(sid);
    co_await drain_to_wire();
    co_return sid;
}

// ════════════════════════════════════════════════════════════════════════════
// CulpeoH2Client implementation
// ════════════════════════════════════════════════════════════════════════════

struct CulpeoH2Client::Impl {
    asio::io_context& ioc;
    asio::ssl::context* tls{nullptr};  // null → cleartext mode
    bool cleartext{false};

    std::shared_ptr<H2Session> session;
    std::unique_ptr<H2Transport> transport_obj;

    explicit Impl(asio::io_context& ioc, asio::ssl::context& ctx)
        : ioc(ioc), tls(&ctx), cleartext(false) {}

    explicit Impl(asio::io_context& ioc, bool)
        : ioc(ioc), cleartext(true) {}
};

CulpeoH2Client::CulpeoH2Client(asio::io_context& ioc, asio::ssl::context& tls)
    : impl_(std::make_unique<Impl>(ioc, tls))
{}

CulpeoH2Client::CulpeoH2Client(asio::io_context& ioc, AllowCleartext)
    : impl_(std::make_unique<Impl>(ioc, /*cleartext=*/true))
{}

CulpeoH2Client::~CulpeoH2Client() = default;

asio::awaitable<void> CulpeoH2Client::connect(std::string host,
                                                std::string port,
                                                std::string path)
{
    auto& ioc = impl_->ioc;
    asio::ip::tcp::resolver resolver(ioc);

    auto endpoints = co_await resolver.async_resolve(
        host, port, asio::use_awaitable);

    if (impl_->cleartext) {
        // Cleartext h2c — direct TCP connection with HTTP/2 Prior Knowledge
        asio::ip::tcp::socket sock(ioc);
        co_await asio::async_connect(sock, endpoints, asio::use_awaitable);

        // Disable Nagle for lower latency
        sock.set_option(asio::ip::tcp::no_delay(true));

        auto session = std::make_shared<H2Session>(
            std::make_unique<TcpSocketStream>(std::move(sock)),
            H2Session::Mode::Client);

        impl_->session = session;

        // Run the session loop in the background
        asio::co_spawn(ioc,
            [session]() -> asio::awaitable<void> {
                co_await session->run();
            },
            asio::detached);

        // Wait for the loop to initialize (run() dispatches to strand first)
        // Give it a brief moment via a post
        co_await asio::post(ioc, asio::use_awaitable);

        int32_t sid = co_await session->submit_request(host, path);
        if (sid < 0)
            throw std::runtime_error("CulpeoH2Client: failed to submit request");

        impl_->transport_obj = std::make_unique<H2Transport>(session, sid);

    } else {
        // TLS mode — ALPN "h2"
        using SslSocket = asio::ssl::stream<asio::ip::tcp::socket>;
        SslSocket ssl_sock(ioc, *impl_->tls);

        co_await asio::async_connect(ssl_sock.lowest_layer(), endpoints,
                                     asio::use_awaitable);
        ssl_sock.lowest_layer().set_option(asio::ip::tcp::no_delay(true));

        // Set ALPN
        SSL_set_tlsext_host_name(ssl_sock.native_handle(), host.c_str());
        // SEC-023: do NOT override verify mode here.  The ssl::context passed
        // by the caller already carries the verify policy (verify_peer for
        // production, verify_none for test fixtures that configure it explicitly).
        SSL_CTX_set_alpn_protos(
            SSL_get_SSL_CTX(ssl_sock.native_handle()),
            reinterpret_cast<const unsigned char*>("\x02h2"), 3);

        co_await ssl_sock.async_handshake(
            asio::ssl::stream_base::client, asio::use_awaitable);

        auto session = std::make_shared<H2Session>(
            std::make_unique<TlsSocketStream>(std::move(ssl_sock)),
            H2Session::Mode::Client);

        impl_->session = session;

        asio::co_spawn(ioc,
            [session]() -> asio::awaitable<void> {
                co_await session->run();
            },
            asio::detached);

        co_await asio::post(ioc, asio::use_awaitable);

        int32_t sid = co_await session->submit_request(host, path);
        if (sid < 0)
            throw std::runtime_error("CulpeoH2Client: failed to submit request");

        impl_->transport_obj = std::make_unique<H2Transport>(session, sid);
    }
}

IAsyncTransport& CulpeoH2Client::transport()
{
    if (!impl_->transport_obj)
        throw std::logic_error("CulpeoH2Client::transport() called before connect()");
    return *impl_->transport_obj;
}

asio::awaitable<std::pair<uint8_t, std::vector<std::byte>>>
CulpeoH2Client::receive_frame()
{
    co_return co_await transport().receive_frame();
}

asio::awaitable<void> CulpeoH2Client::close_session()
{
    if (impl_->session)
        co_await impl_->session->close_session();
}

asio::awaitable<std::shared_ptr<H2Transport>>
CulpeoH2Client::open_additional_stream(std::string path)
{
    if (!impl_->session)
        throw std::logic_error("open_additional_stream() called before connect()");

    // Reuse "localhost" as the :authority header for the additional stream.
    // (A production API would cache the host from connect().)
    std::string host = "localhost";

    int32_t sid = co_await impl_->session->submit_request(host, path);
    if (sid < 0) {
        co_return nullptr; // Server refused (MAX_CONCURRENT_STREAMS exceeded)
    }
    co_return std::make_shared<H2Transport>(impl_->session, sid);
}

} // namespace culpeo::h2
