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
//   struct UserData {
//       std::unique_ptr<culpeo::session::Session>    session;
//       std::unique_ptr<culpeo::transport::WsTransport> transport;
//       std::shared_ptr<std::atomic<bool>>           alive;  // from make_uws_transport
//   };
//
//   app.ws<UserData>("/*", {
//       .open = [](auto* ws) {
//           auto [transport, alive] = culpeo::transport::make_uws_transport(ws);
//           auto* d = ws->getUserData();
//           d->alive     = alive;          // store so .close can reach it
//           d->transport = std::move(transport);
//           d->session   = std::make_unique<culpeo::session::Session>(
//               *d->transport, callbacks, config);
//       },
//       .message = [](auto* ws, std::string_view msg, uWS::OpCode op) {
//           // Parse CulpeoStream frame from msg, then feed to session
//       },
//       // REQUIRED shutdown sequence (CPP-P3-001):
//       //   1. Set alive = false  → stops any in-flight defer() callbacks
//       //   2. session.reset()    → destroys Session (which may call transport)
//       //   3. transport.reset()  → destroys WsTransport (safe: all defers see alive=false)
//       .close = [](auto* ws, int /*code*/, std::string_view /*reason*/) {
//           auto* d = ws->getUserData();
//           d->alive->store(false, std::memory_order_release); // stop deferred sends
//           d->session.reset();    // destroy Session BEFORE transport (CPP-P3-004)
//           d->transport.reset();  // then transport
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
//
// Shutdown safety (CPP-P3-001)
// ────────────────────────────
// Loop::defer() captures a raw uWS::Loop* and a raw uWS::WebSocket*.  If the
// event-loop shuts down or the socket is destroyed before a deferred callback
// fires, invoking the stale pointer is UB/crash.
//
// The alive flag (shared_ptr<atomic<bool>>) solves this:
//   • Every defer() lambda captures alive and checks it before touching ws.
//   • The .close handler sets alive=false BEFORE resetting Session/Transport.
//   • Any lambda that fires after alive=false is a no-op.
//
// The alive flag is returned from make_uws_transport() and MUST be stored in
// UserData and set to false at the start of the .close handler.

#include "culpeo/transport_ws.hpp"

// uWebSockets headers — provided by the consuming project
#include <App.h>
#include <Loop.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace culpeo::transport {

/// Result of make_uws_transport().
/// Store `alive` in your UserData; set it to false at the start of the
/// WebSocket .close handler BEFORE resetting session or transport.
struct UwsTransportResult {
    std::unique_ptr<WsTransport>           transport;
    std::shared_ptr<std::atomic<bool>>     alive;
};

/// Create a WsTransport that wraps a uWebSockets WebSocket* handle.
///
/// @tparam SSL   true for wss:// (uWS::SSLApp), false for ws:// (uWS::App).
/// @param ws     A valid WebSocket pointer, owned by the µWS event loop.
///               The pointer MUST remain valid for the lifetime of the
///               returned WsTransport (i.e. until the .close callback fires).
///
/// Returns a UwsTransportResult containing:
///   - transport: the WsTransport to hand to Session.
///   - alive:     a shared flag that MUST be stored in UserData and set to
///                false at the VERY START of the .close handler to prevent
///                use-after-free on the WebSocket* and Loop* after shutdown.
///
/// The returned transport is safe to use from any thread.  Sends from
/// off-loop threads are posted via uWS::Loop::defer().
template <bool SSL>
UwsTransportResult
make_uws_transport(uWS::WebSocket<SSL, true, void*>* ws) {
    // Shared liveness flag: set to false in the .close handler to ensure
    // no deferred lambda touches a freed WebSocket* or Loop* (CPP-P3-001).
    auto alive = std::make_shared<std::atomic<bool>>(true);

    // Capture the loop so we can defer off-thread sends.
    uWS::Loop* loop = uWS::Loop::get();

    auto send_text = [ws, loop, alive](std::span<const std::byte> frame) {
        // Guard before posting — loop might already be gone.
        if (!alive->load(std::memory_order_acquire)) return;
        // Copy the data — the span may be gone by the time defer() fires.
        std::vector<std::byte> buf(frame.begin(), frame.end());
        loop->defer([ws, buf = std::move(buf), alive]() {
            // Guard again inside the callback — socket may have closed.
            if (!alive->load(std::memory_order_acquire)) return;
            ws->send(
                std::string_view(reinterpret_cast<const char*>(buf.data()), buf.size()),
                uWS::OpCode::TEXT);
        });
    };

    auto send_binary = [ws, loop, alive](std::span<const std::byte> frame) {
        if (!alive->load(std::memory_order_acquire)) return;
        std::vector<std::byte> buf(frame.begin(), frame.end());
        loop->defer([ws, buf = std::move(buf), alive]() {
            if (!alive->load(std::memory_order_acquire)) return;
            ws->send(
                std::string_view(reinterpret_cast<const char*>(buf.data()), buf.size()),
                uWS::OpCode::BINARY);
        });
    };

    auto close_fn = [ws, loop, alive](int code, std::string_view reason) {
        if (!alive->load(std::memory_order_acquire)) return;
        std::string reason_copy(reason);
        loop->defer([ws, code, reason_copy = std::move(reason_copy), alive]() {
            if (!alive->load(std::memory_order_acquire)) return;
            ws->end(code, reason_copy);
        });
    };

    auto transport = std::make_unique<WsTransport>(
        std::move(send_text),
        std::move(send_binary),
        std::move(close_fn));

    return UwsTransportResult{std::move(transport), std::move(alive)};
}

}  // namespace culpeo::transport
