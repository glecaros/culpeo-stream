#pragma once

// CulpeoStream HTTP/2 Server
//
// CulpeoH2Server accepts HTTP/2 connections and dispatches each
// CulpeoStream request to an ISessionHandler coroutine.
//
// Each accepted TCP connection spawns a new H2Session.  Each HTTP/2 stream
// within that connection (one per CulpeoStream session) is dispatched to a
// fresh ISessionHandler::handle() coroutine.
//
// Typical usage:
// @code
//   class EchoHandler : public ISessionHandler {
//       asio::awaitable<void> handle(IAsyncTransport& t) override {
//           while (true) {
//               auto [type, data] = co_await t.receive_frame();
//               if (type == 0x01) co_await t.send_text(data);
//               else              co_await t.send_binary(data);
//           }
//       }
//   };
//
//   asio::io_context ioc;
//   CulpeoH2Server server(ioc, CulpeoH2Server::AllowCleartext{}, 9090,
//                         std::make_shared<EchoHandler>());
//   co_spawn(ioc, server.run(), asio::detached);
//   ioc.run();
// @endcode

#include "culpeo/async_transport.hpp"

#include <asio/io_context.hpp>
#include <asio/ssl/context.hpp>
#include <cstdint>
#include <memory>

namespace culpeo::h2 {

/// Per-session handler interface.
///
/// Implement this to receive and send CulpeoStream frames.
/// The handle() coroutine owns the session lifetime: when handle() returns
/// (or throws), the underlying HTTP/2 stream is closed.
class ISessionHandler {
public:
    virtual ~ISessionHandler() = default;

    /// Handle one CulpeoStream session.
    ///
    /// Called once per accepted HTTP/2 stream (one per CulpeoStream session).
    /// @param transport  Bidirectional transport for this session.
    virtual asio::awaitable<void> handle(IAsyncTransport& transport) = 0;
};

/// HTTP/2 CulpeoStream server.
class CulpeoH2Server {
public:
    /// Tag type enabling cleartext (h2c) mode (for tests).
    struct AllowCleartext {};

    /// Construct a TLS server.
    /// @param ioc      io_context.
    /// @param tls      SSL context (caller loads certificate and private key).
    /// @param port     TCP port to listen on.
    /// @param handler  Session handler factory (shared across all sessions).
    CulpeoH2Server(asio::io_context& ioc,
                   asio::ssl::context& tls,
                   uint16_t port,
                   std::shared_ptr<ISessionHandler> handler);

    /// Construct a cleartext server (for tests / local development).
    CulpeoH2Server(asio::io_context& ioc,
                   AllowCleartext,
                   uint16_t port,
                   std::shared_ptr<ISessionHandler> handler);

    ~CulpeoH2Server();

    // Non-copyable
    CulpeoH2Server(const CulpeoH2Server&) = delete;
    CulpeoH2Server& operator=(const CulpeoH2Server&) = delete;

    /// Accept connections and dispatch to the handler.  Run until cancelled.
    asio::awaitable<void> run();

    /// Stop accepting new connections.  Closes the acceptor so run() exits.
    /// Existing sessions continue until they close naturally.
    void stop();

    /// Return the actual TCP port (useful when port 0 is passed for OS selection).
    uint16_t port() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace culpeo::h2
