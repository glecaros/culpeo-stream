// h2_server.cpp — CulpeoH2Server implementation

#include "culpeo/h2_server.hpp"
#include "culpeo/h2_transport.hpp"
#include "h2_session.hpp"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/dispatch.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/use_awaitable.hpp>

#include <stdexcept>

// OpenSSL ALPN callback
#include <openssl/ssl.h>

namespace culpeo::h2 {

// ════════════════════════════════════════════════════════════════════════════
// CulpeoH2Server::Impl
// ════════════════════════════════════════════════════════════════════════════

struct CulpeoH2Server::Impl {
    asio::io_context& ioc;
    asio::ssl::context* tls{nullptr};
    bool cleartext{false};
    uint16_t port{0};
    std::shared_ptr<ISessionHandler> handler;

    asio::ip::tcp::acceptor acceptor;
    uint16_t actual_port{0};

    Impl(asio::io_context& ioc, asio::ssl::context& ctx, uint16_t p,
         std::shared_ptr<ISessionHandler> h)
        : ioc(ioc), tls(&ctx), cleartext(false), port(p)
        , handler(std::move(h))
        , acceptor(ioc)
    {}

    Impl(asio::io_context& ioc, bool, uint16_t p,
         std::shared_ptr<ISessionHandler> h)
        : ioc(ioc), cleartext(true), port(p)
        , handler(std::move(h))
        , acceptor(ioc)
    {}

    void start_listen() {
        asio::ip::tcp::endpoint ep(asio::ip::tcp::v4(), port);
        acceptor.open(ep.protocol());
        acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
        acceptor.bind(ep);
        acceptor.listen();
        actual_port = acceptor.local_endpoint().port();
    }
};

// ════════════════════════════════════════════════════════════════════════════
// Constructors
// ════════════════════════════════════════════════════════════════════════════

CulpeoH2Server::CulpeoH2Server(asio::io_context& ioc,
                                asio::ssl::context& tls,
                                uint16_t port,
                                std::shared_ptr<ISessionHandler> handler)
    : impl_(std::make_unique<Impl>(ioc, tls, port, std::move(handler)))
{
    impl_->start_listen();
}

CulpeoH2Server::CulpeoH2Server(asio::io_context& ioc,
                                AllowCleartext,
                                uint16_t port,
                                std::shared_ptr<ISessionHandler> handler)
    : impl_(std::make_unique<Impl>(ioc, /*cleartext*/true, port, std::move(handler)))
{
    impl_->start_listen();
}

CulpeoH2Server::~CulpeoH2Server() = default;

uint16_t CulpeoH2Server::port() const
{
    return impl_->actual_port;
}

void CulpeoH2Server::stop()
{
    asio::error_code ec;
    impl_->acceptor.close(ec); // Causes async_accept to fail → run() exits
}

// ════════════════════════════════════════════════════════════════════════════
// run() — accept loop
// ════════════════════════════════════════════════════════════════════════════

asio::awaitable<void> CulpeoH2Server::run()
{
    auto& ioc = impl_->ioc;

    while (true) {
        asio::ip::tcp::socket raw_sock(ioc);

        try {
            co_await impl_->acceptor.async_accept(raw_sock, asio::use_awaitable);
        } catch (const asio::system_error&) {
            // Acceptor closed — stop loop
            break;
        }

        raw_sock.set_option(asio::ip::tcp::no_delay(true));

        if (impl_->cleartext) {
            // Cleartext h2c
            auto session = std::make_shared<H2Session>(
                std::make_unique<TcpSocketStream>(std::move(raw_sock)),
                H2Session::Mode::Server);

            auto handler_ptr = impl_->handler;

            // IMPORTANT: capture weak_ptr to avoid a circular reference:
            // H2Session → new_stream_cb_ (lambda) → shared_ptr<H2Session>
            // would prevent the H2Session from ever being destroyed.
            std::weak_ptr<H2Session> weak = session;

            // Register handler for new streams
            session->set_new_stream_handler(
                [handler_ptr, weak](int32_t stream_id) -> asio::awaitable<void> {
                    auto sess = weak.lock();
                    if (!sess) co_return;
                    auto transport = std::make_shared<H2Transport>(sess, stream_id);
                    sess = nullptr; // release our ref; H2Transport keeps session alive
                    try {
                        co_await handler_ptr->handle(*transport);
                    } catch (...) {
                        // Handler threw; stream will be closed below
                    }
                    // Close the stream when handler exits (H2Transport holds session ref)
                    co_await transport->close(0, "");
                });

            // Spawn session read loop
            asio::co_spawn(ioc,
                [session]() -> asio::awaitable<void> {
                    co_await session->run();
                },
                asio::detached);

        } else {
            // TLS mode
            using SslSocket = asio::ssl::stream<asio::ip::tcp::socket>;
            auto ssl_sock = std::make_shared<SslSocket>(std::move(raw_sock), *impl_->tls);

            auto handler_ptr = impl_->handler;

            asio::co_spawn(ioc,
                [ssl_sock, handler_ptr, &ioc]() -> asio::awaitable<void> {
                    // ALPN: server side
                    // (SSL context must have ALPN protos set by caller)
                    try {
                        co_await ssl_sock->async_handshake(
                            asio::ssl::stream_base::server, asio::use_awaitable);
                    } catch (...) {
                        co_return; // TLS handshake failed; drop connection
                    }

                    auto session = std::make_shared<H2Session>(
                        std::make_unique<TlsSocketStream>(std::move(*ssl_sock)),
                        H2Session::Mode::Server);

                    std::weak_ptr<H2Session> weak = session;

                    session->set_new_stream_handler(
                        [handler_ptr, weak](int32_t stream_id) -> asio::awaitable<void> {
                            auto sess = weak.lock();
                            if (!sess) co_return;
                            auto transport = std::make_shared<H2Transport>(sess, stream_id);
                            sess = nullptr;
                            try {
                                co_await handler_ptr->handle(*transport);
                            } catch (...) {}
                            co_await transport->close(0, "");
                        });

                    co_await session->run();
                },
                asio::detached);
        }
    }
}

} // namespace culpeo::h2
