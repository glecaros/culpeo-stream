#pragma once

// CulpeoStream HTTP/2 Transport — H2Transport
//
// H2Transport implements IAsyncTransport over a single HTTP/2 stream.
// It wraps an internal H2Session (one per TCP connection) and a stream ID.
//
// Usage (server-side, within ISessionHandler::handle):
// @code
//   asio::awaitable<void> MyHandler::handle(IAsyncTransport& t) {
//       auto [type, data] = co_await t.receive_frame();   // wait for culpeo.init
//       // …process frame…
//       co_await t.send_text(response_bytes);
//   }
// @endcode
//
// Thread safety
// ─────────────
// All methods dispatch to an Asio strand; callers do not need to serialise.
//
// Buffer lifetime
// ───────────────
// Spans passed to send_text / send_binary are copied internally before the
// co_await that writes to the wire.

#include "culpeo/async_transport.hpp"

#include <asio/any_io_executor.hpp>
#include <asio/strand.hpp>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace culpeo::h2 {

// Forward-declared internal type; definition in src/h2_session.hpp
class H2Session;

/// Concrete async transport over one HTTP/2 stream.
///
/// Obtain instances via CulpeoH2Client::transport() or through the
/// CulpeoH2Server dispatch path.
///
/// H2Transport is NOT default-constructible; it is always created by
/// CulpeoH2Client or CulpeoH2Server.
class H2Transport final : public culpeo::IAsyncTransport {
public:
    /// Construct with an existing H2Session and stream ID.
    /// @param session   Shared ownership of the H2Session.
    /// @param stream_id The HTTP/2 stream ID this transport wraps.
    H2Transport(std::shared_ptr<H2Session> session, int32_t stream_id);

    ~H2Transport() override;

    // IAsyncTransport implementation ─────────────────────────────────────────

    /// Prepend type octet 0x01 + 4-byte BE length, enqueue for nghttp2 DATA send.
    asio::awaitable<void> send_text(
        std::span<const std::byte> frame) override;

    /// Prepend type octet 0x02 + 4-byte BE length, enqueue for nghttp2 DATA send.
    asio::awaitable<void> send_binary(
        std::span<const std::byte> frame) override;

    /// Send RST_STREAM (code 1002/1008) or GOAWAY (code 1000) and close.
    asio::awaitable<void> close(int code, std::string_view reason) override;

    /// Wait for the next complete CulpeoStream frame on this stream.
    /// Returns {type_byte, payload}.  Throws on EOF / stream close.
    asio::awaitable<std::pair<uint8_t, std::vector<std::byte>>>
    receive_frame() override;

    /// Return a captured HTTP/2 request header value.
    /// SEC-028: exposes "authorization", "content-type", etc.
    /// @param name  Lowercase header name.
    std::string request_header(std::string_view name) const override;

private:
    std::shared_ptr<H2Session> session_;
    int32_t stream_id_;
};

} // namespace culpeo::h2
