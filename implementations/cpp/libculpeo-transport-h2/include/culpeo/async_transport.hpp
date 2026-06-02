#pragma once

// CulpeoStream Async Transport Interface
//
// IAsyncTransport is the coroutine-based transport abstraction used by
// libculpeo-transport-h2.  It is PARALLEL to ITransport (the synchronous
// callback-based interface used by libculpeo-session / libculpeo-transport-ws)
// and deliberately does NOT inherit from it.
//
// Design rationale
// ─────────────────
// HTTP/2 transport requires:
//   • Natural backpressure on sends (flow-control windows)
//   • Error propagation from sends (co_await returns error_code)
//   • Bidirectional framing within a single HTTP/2 stream
//
// These are all naturally expressed as coroutines.  Patching them onto the
// synchronous ITransport callback API would require a callback-to-coroutine
// bridge that adds complexity without benefit.
//
// receive_frame() deviation from instructions
// ─────────────────────────────────────────────
// The agent instructions specify only send_text / send_binary / close.
// IAsyncTransport adds receive_frame() because ISessionHandler::handle()
// needs to receive as well as send — a send-only interface cannot implement
// any real session lifecycle (including the required echo tests).  The
// addition is documented in DECISIONS.md.
//
// Executor
// ────────
// Implementations use asio::strand<asio::any_io_executor> to serialise sends.
// All co_await expressions within an IAsyncTransport implementation MUST
// resume on the strand.

#include <asio/awaitable.hpp>
#include <asio/error_code.hpp>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace culpeo {

/// Async bidirectional transport interface (Asio coroutine-based).
///
/// All methods are coroutine-safe and MUST be awaited from within an
/// asio::io_context-driven coroutine.
class IAsyncTransport {
public:
    virtual ~IAsyncTransport() = default;

    // Non-copyable, non-movable (owns internal state).
    IAsyncTransport(const IAsyncTransport&)            = delete;
    IAsyncTransport& operator=(const IAsyncTransport&) = delete;
    IAsyncTransport(IAsyncTransport&&)                 = delete;
    IAsyncTransport& operator=(IAsyncTransport&&)      = delete;

    /// Send a control (text) frame.
    ///
    /// The type octet 0x01 is prepended automatically.
    /// @param frame  Raw CulpeoStream frame bytes (header block + body).
    ///               Only valid for the duration of the call; implementations
    ///               MUST copy if they buffer.
    virtual asio::awaitable<void> send_text(
        std::span<const std::byte> frame) = 0;

    /// Send a media (binary) frame.
    ///
    /// The type octet 0x02 is prepended automatically.
    /// @param frame  Raw CulpeoStream frame bytes.
    virtual asio::awaitable<void> send_binary(
        std::span<const std::byte> frame) = 0;

    /// Close the transport.
    ///
    /// For HTTP/2: sends a GOAWAY or RST_STREAM as appropriate.
    /// For WebSocket: sends a Close frame with the given code and reason.
    ///
    /// @param code    WebSocket-compatible close code (1000 = normal, 1002 =
    ///                protocol error, 1008 = policy violation).
    /// @param reason  Human-readable close reason.
    virtual asio::awaitable<void> close(
        int code, std::string_view reason) = 0;

    /// Receive the next complete CulpeoStream frame.
    ///
    /// Returns {type_byte, payload} where:
    ///   type_byte == 0x01  → control frame (culpeo.init, culpeo.ping, …)
    ///   type_byte == 0x02  → media frame
    ///
    /// Throws asio::error::eof (or co_returns with an exception) when the
    /// remote side closes the stream cleanly.
    ///
    /// @return Pair of {type_byte, frame_payload}.
    virtual asio::awaitable<std::pair<uint8_t, std::vector<std::byte>>>
    receive_frame() = 0;

    /// Return the value of an HTTP-level request header captured during the
    /// initial HEADERS frame exchange.
    ///
    /// SEC-028: exposes security-relevant headers (e.g. "authorization",
    /// "content-type") so application handlers can authenticate requests at
    /// the transport layer.
    ///
    /// The default implementation returns an empty string; H2Transport
    /// overrides this to expose headers captured from the HTTP/2 HEADERS frame.
    ///
    /// @param name  Lowercase header name (e.g. "authorization").
    /// @return Header value, or empty string if not available.
    virtual std::string request_header(std::string_view /*name*/) const {
        return {};
    }

protected:
    IAsyncTransport() = default;
};

} // namespace culpeo
