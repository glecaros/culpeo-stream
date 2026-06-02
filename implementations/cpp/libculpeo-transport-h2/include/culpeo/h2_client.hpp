#pragma once

// CulpeoStream HTTP/2 Client
//
// CulpeoH2Client opens a CulpeoStream session over HTTP/2 as described in
// Addendum C of the CulpeoStream Protocol Specification v0.3.0.
//
// Typical usage (cleartext, for tests):
// @code
//   asio::io_context ioc;
//   CulpeoH2Client client(ioc, CulpeoH2Client::AllowCleartext{});
//   co_await client.connect("127.0.0.1", "9090", "/");
//   auto& t = client.transport();
//   co_await t.send_text(init_frame);
//   auto [type, data] = co_await t.receive_frame();
// @endcode
//
// Typical usage (TLS):
// @code
//   asio::io_context ioc;
//   asio::ssl::context tls(asio::ssl::context::tls_client);
//   tls.set_verify_mode(asio::ssl::verify_peer);
//   tls.set_default_verify_paths();
//   CulpeoH2Client client(ioc, tls);
//   co_await client.connect("example.com", "443", "/stream");
// @endcode

#include "culpeo/h2_transport.hpp"
#include "culpeo/async_transport.hpp"

#include <asio/io_context.hpp>
#include <asio/ssl/context.hpp>
#include <cstdint>
#include <memory>
#include <string>

namespace culpeo::h2 {

/// HTTP/2 CulpeoStream client.
///
/// One instance per connection.  Not thread-safe: all operations must run
/// within the asio::io_context provided at construction.
class CulpeoH2Client {
public:
    /// Tag type enabling cleartext (h2c) connections.
    /// Used in tests and local development only; TLS is required in production
    /// (spec §C.5).
    struct AllowCleartext {};

    /// Construct a TLS client.
    /// @param ioc  io_context to run coroutines on.
    /// @param tls  SSL context (caller sets verify mode, CA paths, etc.).
    CulpeoH2Client(asio::io_context& ioc, asio::ssl::context& tls);

    /// Construct a cleartext client (AllowCleartext tag).
    /// For tests and local development only.
    CulpeoH2Client(asio::io_context& ioc, AllowCleartext);

    ~CulpeoH2Client();

    // Non-copyable
    CulpeoH2Client(const CulpeoH2Client&) = delete;
    CulpeoH2Client& operator=(const CulpeoH2Client&) = delete;

    /// Open a TCP connection, complete TLS handshake (ALPN "h2"), send
    /// HTTP/2 client preface + SETTINGS, and submit a POST request to path.
    ///
    /// After this coroutine returns, transport() is valid.
    ///
    /// @param host  Hostname or IP address.
    /// @param port  Port number as a string (e.g., "443", "9090").
    /// @param path  Request path (e.g., "/").
    asio::awaitable<void> connect(
        std::string host, std::string port, std::string path);

    /// Access the transport for the established connection.
    /// Only valid after connect() completes successfully.
    IAsyncTransport& transport();

    /// Close the entire HTTP/2 connection (sends GOAWAY + closes socket).
    /// This causes the server-side session to exit cleanly.
    asio::awaitable<void> close_session();

    /// Convenience wrapper: read one CulpeoStream frame from the response stream.
    /// Equivalent to transport().receive_frame().
    asio::awaitable<std::pair<uint8_t, std::vector<std::byte>>> receive_frame();

    /// Open an additional HTTP/2 stream on the same TCP connection.
    ///
    /// Returns a new H2Transport for the additional stream, or nullptr if the
    /// server refused the stream (e.g., SETTINGS_MAX_CONCURRENT_STREAMS limit
    /// was reached per SEC-025).
    ///
    /// @param path  Request path (e.g., "/").
    asio::awaitable<std::shared_ptr<H2Transport>> open_additional_stream(
        std::string path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace culpeo::h2
