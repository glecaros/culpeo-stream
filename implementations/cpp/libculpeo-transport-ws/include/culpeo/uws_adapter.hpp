#pragma once

// CulpeoStream — uWebSockets adapter
//
// Include this header AFTER both uWebSockets and transport_ws.hpp.
//
// Requires:
//   - uWebSockets headers on the include path (App.h, WebSocket.h, Loop.h)
//   - uSockets library linked into the final binary
//
// This file is intentionally header-only so that the core culpeo_transport_ws
// library does not carry a mandatory compile-time dependency on uWebSockets.
//
// Usage (inside a uWS .open handler):
//
//   app.ws<SessionData>("/*", {
//       .open = [](auto* ws) {
//           auto transport = culpeo::transport::make_uws_transport(ws);
//           ws->getUserData()->transport = std::move(transport);
//           ws->getUserData()->session =
//               std::make_unique<culpeo::session::Session>(
//                   *ws->getUserData()->transport, callbacks, config);
//       },
//       .message = [](auto* ws, std::string_view msg, uWS::OpCode op) {
//           // Parse CulpeoStream frame from msg, then feed to session
//       },
//       .close = [](auto* ws, int /*code*/, std::string_view /*reason*/) {
//           ws->getUserData()->session.reset();
//           ws->getUserData()->transport.reset();
//       }
//   });
//
// Thread-safety note
// ──────────────────
// µWS is NOT thread-safe: send/close on a uWS::WebSocket* MUST be called
// from the event-loop thread that owns the socket.  When the CulpeoStream
// session layer invokes transport methods from an off-loop thread (e.g. via
// Session::send_media() called from a worker), the send must be deferred to
// the event-loop.
//
// make_uws_transport() addresses this by capturing the uWS::Loop* and using
// Loop::defer() for off-loop sends.  If the calling thread IS the event-loop
// thread, the send executes synchronously inside the defer callback on the
// next loop iteration.  This is slightly delayed but always correct.
//
// For latency-critical applications that always call Session::send_media()
// from within uWS callbacks (i.e. always on the loop thread), you can
// replace the defer() wrappers below with direct ws->send() calls.

#include "culpeo/transport_ws.hpp"

// uWebSockets headers — provided by the consuming project
#include <App.h>
#include <Loop.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace culpeo::transport {

/// Create a WsTransport that wraps a uWebSockets WebSocket* handle.
///
/// @tparam SSL   true for wss:// (uWS::SSLApp), false for ws:// (uWS::App).
/// @param ws     A valid WebSocket pointer, owned by the µWS event loop.
///               The pointer MUST remain valid for the lifetime of the
///               returned WsTransport (i.e. until the .close callback fires).
///
/// The returned transport is safe to use from any thread.  Sends from
/// off-loop threads are posted via uWS::Loop::defer().
template <bool SSL>
std::unique_ptr<WsTransport>
make_uws_transport(uWS::WebSocket<SSL, true, void*>* ws) {
    // Capture the loop so we can defer off-thread sends.
    uWS::Loop* loop = uWS::Loop::get();

    auto send_text = [ws, loop](std::span<const std::byte> frame) {
        // Copy the data — the span may be gone by the time defer() fires.
        std::vector<std::byte> buf(frame.begin(), frame.end());
        loop->defer([ws, buf = std::move(buf)]() {
            ws->send(
                std::string_view(reinterpret_cast<const char*>(buf.data()), buf.size()),
                uWS::OpCode::TEXT);
        });
    };

    auto send_binary = [ws, loop](std::span<const std::byte> frame) {
        std::vector<std::byte> buf(frame.begin(), frame.end());
        loop->defer([ws, buf = std::move(buf)]() {
            ws->send(
                std::string_view(reinterpret_cast<const char*>(buf.data()), buf.size()),
                uWS::OpCode::BINARY);
        });
    };

    auto close_fn = [ws, loop](int code, std::string_view reason) {
        std::string reason_copy(reason);
        loop->defer([ws, code, reason_copy = std::move(reason_copy)]() {
            ws->end(code, reason_copy);
        });
    };

    return std::make_unique<WsTransport>(
        std::move(send_text),
        std::move(send_binary),
        std::move(close_fn));
}

}  // namespace culpeo::transport
