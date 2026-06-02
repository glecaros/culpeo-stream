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

/// Configuration options for CulpeoH2Server.
struct CulpeoH2ServerOptions {
    /// Maximum number of concurrent HTTP/2 streams per connection.
    /// Sent as SETTINGS_MAX_CONCURRENT_STREAMS; also enforced server-side
    /// against clients that ignore SETTINGS (SEC-025).
    uint32_t max_concurrent_streams{100};

    /// TLS handshake timeout in seconds.
    /// If a client completes TCP but stalls the TLS handshake for longer
    /// than this, the connection is dropped (SEC-029).
    uint32_t handshake_timeout_seconds{10};
};

/// HTTP/2 CulpeoStream server.
class CulpeoH2Server {
public:
    /// Tag type enabling cleartext (h2c) mode (for tests).
    struct AllowCleartext {};

    /// Construct a TLS server with default options.
    /// @param ioc      io_context.
    /// @param tls      SSL context (caller loads certificate and private key).
    /// @param port     TCP port to listen on.
    /// @param handler  Session handler factory (shared across all sessions).
    CulpeoH2Server(asio::io_context& ioc,
                   asio::ssl::context& tls,
                   uint16_t port,
                   std::shared_ptr<ISessionHandler> handler);

    /// Construct a TLS server with explicit options.
    CulpeoH2Server(asio::io_context& ioc,
                   asio::ssl::context& tls,
                   uint16_t port,
                   std::shared_ptr<ISessionHandler> handler,
                   CulpeoH2ServerOptions opts);

    /// Construct a cleartext server (for tests / local development).
    CulpeoH2Server(asio::io_context& ioc,
                   AllowCleartext,
                   uint16_t port,
                   std::shared_ptr<ISessionHandler> handler);

    /// Construct a cleartext server with explicit options.
    CulpeoH2Server(asio::io_context& ioc,
                   AllowCleartext,
                   uint16_t port,
                   std::shared_ptr<ISessionHandler> handler,
                   CulpeoH2ServerOptions opts);

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
