#pragma once

// CulpeoStream WebSocket Transport Adapter
//
// This header provides WsTransport — a concrete implementation of
// culpeo::session::ITransport that wraps three std::function callbacks
// (send_text, send_binary, close).
//
// Design rationale
// ─────────────────
// Rather than hard-wiring a specific WebSocket library, WsTransport receives
// its I/O operations via injected callables.  This makes:
//
//   • Unit testing trivial  — pass lambda mocks; no network I/O.
//   • Library integration clean — see uws_adapter.hpp for a ready-made
//     factory that wraps a uWebSockets WebSocket* handle.
//
// Thread safety
// ─────────────
// send_text() and send_binary() are serialized under an internal mutex.
// This allows the session layer to call them from any thread.
//
// IMPORTANT for uWebSockets users: µWS is NOT thread-safe.  All calls that
// touch a uWS::WebSocket* must happen on the event-loop thread.  The
// uws_adapter.hpp factory demonstrates how to post sends to the loop via
// uWS::Loop::get()->defer() when calling from an off-loop thread.
//
// Buffer lifetime
// ───────────────
// The span passed to send_text / send_binary is only guaranteed valid for
// the duration of the call.  Callables that perform asynchronous sends MUST
// copy the data before returning.

#include "culpeo/session.hpp"

#include <functional>
#include <mutex>
#include <span>
#include <string_view>

namespace culpeo::transport {

/// Thread-safe WebSocket transport adapter backed by injected callbacks.
///
/// Typical server-side usage (uWebSockets):
/// @code
///   #include <culpeo/uws_adapter.hpp>
///   // inside uWS .open handler:
///   auto transport = culpeo::transport::make_uws_transport(ws);
///   ws->getUserData()->session =
///       std::make_unique<culpeo::session::Session>(*transport, callbacks, config);
/// @endcode
///
/// Typical test usage:
/// @code
///   std::vector<std::string> sent_text;
///   WsTransport t(
///       [&](auto frame) { sent_text.emplace_back(as_string(frame)); },
///       [](auto) {},
///       [](int, std::string_view) {}
///   );
/// @endcode
class WsTransport : public culpeo::session::ITransport {
public:
    /// Callable type for text/binary sends.
    /// The span is only valid for the duration of the call.
    using SendFn = std::function<void(std::span<const std::byte>)>;

    /// Callable type for connection close.
    /// @param code    WebSocket close code (RFC 6455 §7.4.1).
    /// @param reason  Human-readable reason (SHOULD be ≤ 123 UTF-8 bytes).
    using CloseFn = std::function<void(int code, std::string_view reason)>;

    /// Construct a WsTransport with explicit send/close callables.
    ///
    /// @param send_text_fn    Called by send_text().   Must not be null.
    /// @param send_binary_fn  Called by send_binary(). Must not be null.
    /// @param close_fn        Called by close().       Must not be null.
    WsTransport(SendFn send_text_fn, SendFn send_binary_fn, CloseFn close_fn);

    ~WsTransport() override = default;

    // Non-copyable, non-movable (owns a mutex).
    WsTransport(const WsTransport&)            = delete;
    WsTransport& operator=(const WsTransport&) = delete;
    WsTransport(WsTransport&&)                 = delete;
    WsTransport& operator=(WsTransport&&)      = delete;

    // ── ITransport implementation ─────────────────────────────────────────────

    /// Send a text (control/event) WebSocket frame.
    /// Thread-safe; serialized via internal mutex.
    void send_text(std::span<const std::byte> frame) override;

    /// Send a binary (media) WebSocket frame.
    /// Thread-safe; serialized via internal mutex.
    void send_binary(std::span<const std::byte> frame) override;

    /// Close the WebSocket connection with the given status code and reason.
    /// Thread-safe; serialized via internal mutex.
    ///
    /// Standard codes used by the session layer:
    ///   1000 — Normal Closure
    ///   1002 — Protocol Error   (CulpeoStream "protocol-error")
    ///   1008 — Policy Violation (CulpeoStream "unauthorized" / "auth-expired")
    void close(int code, std::string_view reason) override;

private:
    std::mutex mu_;
    SendFn     send_text_fn_;
    SendFn     send_binary_fn_;
    CloseFn    close_fn_;
};

}  // namespace culpeo::transport
