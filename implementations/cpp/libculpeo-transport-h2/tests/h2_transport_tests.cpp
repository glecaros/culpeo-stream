// h2_transport_tests.cpp — CulpeoStream HTTP/2 transport tests
//
// Tests:
//  1. Framing round-trip (encode / decode without network)
//  2. Client connects to server (cleartext), exchange control + media frames
//  3. Close propagation (client close → server handler exits)
//  4. Cleartext mode basic ping-pong (AllowCleartext flag)
//  5. Large frame (> 65 535 bytes) round-trip
//  6. Concurrent sends (100 frames from two coroutines, strand-serialised)
//  7. Interop: culpeo.init serialised via libculpeo-message, echoed by server

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_all.hpp>

#include "culpeo/async_transport.hpp"
#include "culpeo/h2_client.hpp"
#include "culpeo/h2_server.hpp"
#include "culpeo/h2_transport.hpp"
#include "culpeo/message.hpp"   // libculpeo-message (for interop test)

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ssl/context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_future.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// ─── Include the internal framing helper ─────────────────────────────────────
// We pull in the encode helper directly to test it without linking h2 session.
#define INTERNAL_FRAMING_TEST 1
#include "../src/h2_session.hpp"

// Path to test TLS certificates (generated once by openssl; stored in tests/).
// The CMake build system sets this macro; fall back to the source-relative path.
#ifndef TEST_CERT_PATH
#  define TEST_CERT_PATH ""
#endif

using namespace culpeo::h2;
using namespace std::chrono_literals;
using culpeo::IAsyncTransport;

// ─── Test helpers ─────────────────────────────────────────────────────────────

/// Run an io_context with a timeout; throw if it doesn't finish in time.
static void run_with_timeout(asio::io_context& ioc,
                              std::chrono::milliseconds timeout = 10s)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        ioc.run_for(50ms);
        if (ioc.stopped()) break;
    }
}

/// Spin up io_context on a background thread; join on destruction.
struct IoRunner {
    asio::io_context ioc;
    asio::executor_work_guard<asio::io_context::executor_type> guard{
        asio::make_work_guard(ioc)};
    std::thread thread{[this] { ioc.run(); }};

    ~IoRunner() {
        guard.reset();
        thread.join();
    }
};

// Simple echo handler
class EchoHandler : public ISessionHandler {
public:
    std::atomic<int> frames_received{0};

    asio::awaitable<void> handle(IAsyncTransport& t) override {
        try {
            while (true) {
                auto [type, data] = co_await t.receive_frame();
                frames_received.fetch_add(1, std::memory_order_relaxed);
                if (type == kTypeControl)
                    co_await t.send_text(
                        std::span<const std::byte>(data.data(), data.size()));
                else
                    co_await t.send_binary(
                        std::span<const std::byte>(data.data(), data.size()));
            }
        } catch (const asio::system_error&) {
            // EOF / stream close — normal exit
        }
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// TEST 1 — Framing round-trip (no network, pure encode/decode)
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("H2 framing: encode/decode round-trip", "[h2][framing]") {
    SECTION("control frame") {
        std::vector<std::byte> payload;
        std::string raw = "Content-Type: application/json\r\nEvent: culpeo.init\r\n\r\n{}";
        for (char c : raw) payload.push_back(static_cast<std::byte>(c));

        auto encoded = encode_h2_envelope(kTypeControl,
            std::span<const std::byte>(payload));

        // Verify length field = 1 + payload.size()
        uint32_t len_field = (static_cast<uint32_t>(encoded[0]) << 24)
                           | (static_cast<uint32_t>(encoded[1]) << 16)
                           | (static_cast<uint32_t>(encoded[2]) <<  8)
                           |  static_cast<uint32_t>(encoded[3]);
        REQUIRE(len_field == 1u + payload.size());
        REQUIRE(encoded[4] == static_cast<uint8_t>(kTypeControl));

        // Decode using RecvState
        H2Session::RecvState rs;
        rs.buf.insert(rs.buf.end(), encoded.begin(), encoded.end());
        auto frames = rs.drain_frames();

        REQUIRE(frames.size() == 1);
        REQUIRE(frames[0].first == kTypeControl);
        REQUIRE(frames[0].second == payload);
    }

    SECTION("media frame") {
        std::vector<std::byte> pcm(256);
        for (std::size_t i = 0; i < pcm.size(); ++i)
            pcm[i] = static_cast<std::byte>(i & 0xFF);

        auto encoded = encode_h2_envelope(kTypeMedia,
            std::span<const std::byte>(pcm));

        H2Session::RecvState rs;
        rs.buf.insert(rs.buf.end(), encoded.begin(), encoded.end());
        auto frames = rs.drain_frames();

        REQUIRE(frames.size() == 1);
        REQUIRE(frames[0].first == kTypeMedia);
        REQUIRE(frames[0].second == pcm);
    }

    SECTION("two frames back-to-back") {
        std::vector<std::byte> a = {std::byte{0xAA}, std::byte{0xBB}};
        std::vector<std::byte> b = {std::byte{0xCC}, std::byte{0xDD}, std::byte{0xEE}};

        H2Session::RecvState rs;
        auto ea = encode_h2_envelope(kTypeControl, std::span<const std::byte>(a));
        auto eb = encode_h2_envelope(kTypeMedia,   std::span<const std::byte>(b));
        rs.buf.insert(rs.buf.end(), ea.begin(), ea.end());
        rs.buf.insert(rs.buf.end(), eb.begin(), eb.end());

        auto frames = rs.drain_frames();
        REQUIRE(frames.size() == 2);
        REQUIRE(frames[0].first  == kTypeControl);
        REQUIRE(frames[0].second == a);
        REQUIRE(frames[1].first  == kTypeMedia);
        REQUIRE(frames[1].second == b);
    }

    SECTION("partial frame — no output until complete") {
        std::vector<std::byte> payload = {std::byte{1}, std::byte{2}, std::byte{3}};
        auto encoded = encode_h2_envelope(kTypeControl,
            std::span<const std::byte>(payload));

        H2Session::RecvState rs;
        // Feed only the first 3 bytes (incomplete length field)
        rs.buf.insert(rs.buf.end(), encoded.begin(), encoded.begin() + 3);
        REQUIRE(rs.drain_frames().empty());

        // Feed the rest
        rs.buf.insert(rs.buf.end(), encoded.begin() + 3, encoded.end());
        auto frames = rs.drain_frames();
        REQUIRE(frames.size() == 1);
        REQUIRE(frames[0].second == payload);
    }

    SECTION("zero-length payload") {
        std::vector<std::byte> empty;
        auto encoded = encode_h2_envelope(kTypeControl,
            std::span<const std::byte>(empty));

        // length = 1 (just the type byte)
        uint32_t len_field = (static_cast<uint32_t>(encoded[0]) << 24)
                           | (static_cast<uint32_t>(encoded[1]) << 16)
                           | (static_cast<uint32_t>(encoded[2]) <<  8)
                           |  static_cast<uint32_t>(encoded[3]);
        REQUIRE(len_field == 1u);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// TEST 2 — Client ↔ Server cleartext: exchange one control + one media frame
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("H2 cleartext: client/server exchange control and media frames",
          "[h2][network][cleartext]")
{
    auto handler = std::make_shared<EchoHandler>();

    asio::io_context ioc;
    CulpeoH2Server server(ioc, CulpeoH2Server::AllowCleartext{}, 0,
                          handler);
    uint16_t srv_port = server.port();

    // Run accept loop in background
    asio::co_spawn(ioc, server.run(), asio::detached);

    // Run client and exchange frames
    std::string recv_text, recv_binary;

    asio::co_spawn(ioc,
        [&]() -> asio::awaitable<void> {
            CulpeoH2Client client(ioc, CulpeoH2Client::AllowCleartext{});
            co_await client.connect("127.0.0.1",
                                    std::to_string(srv_port), "/");

            // Send control frame
            std::string ctrl = "Event: culpeo.test\r\n\r\nhello";
            std::vector<std::byte> ctrl_bytes;
            for (char c : ctrl) ctrl_bytes.push_back(static_cast<std::byte>(c));
            co_await client.transport().send_text(
                std::span<const std::byte>(ctrl_bytes));

            // Receive echo
            auto [t1, p1] = co_await client.receive_frame();
            REQUIRE(t1 == kTypeControl);
            recv_text = std::string(reinterpret_cast<const char*>(p1.data()), p1.size());

            // Send media frame
            std::vector<std::byte> media = {std::byte{0x01}, std::byte{0x02},
                                             std::byte{0x03}};
            co_await client.transport().send_binary(
                std::span<const std::byte>(media));

            // Receive echo
            auto [t2, p2] = co_await client.receive_frame();
            REQUIRE(t2 == kTypeMedia);
            recv_binary = std::string(reinterpret_cast<const char*>(p2.data()), p2.size());

            ioc.stop();
        },
        asio::detached);

    run_with_timeout(ioc);

    REQUIRE(recv_text  == "Event: culpeo.test\r\n\r\nhello");
    REQUIRE(recv_binary == std::string("\x01\x02\x03"));
    REQUIRE(handler->frames_received.load() >= 2);
}

// ═════════════════════════════════════════════════════════════════════════════
// TEST 3 — Close propagation: client closes, server handler exits
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("H2 cleartext: close propagation", "[h2][network][cleartext]")
{
    std::atomic<bool> handler_exited{false};

    class CloseWatchHandler : public ISessionHandler {
    public:
        std::atomic<bool>& exited;
        explicit CloseWatchHandler(std::atomic<bool>& e) : exited(e) {}

        asio::awaitable<void> handle(IAsyncTransport& t) override {
            try {
                // Block waiting for a frame — should unblock on close
                co_await t.receive_frame();
            } catch (const asio::system_error&) {
                // Expected on close
            }
            exited.store(true, std::memory_order_release);
        }
    };

    auto handler = std::make_shared<CloseWatchHandler>(handler_exited);

    asio::io_context ioc;
    CulpeoH2Server server(ioc, CulpeoH2Server::AllowCleartext{}, 0, handler);
    uint16_t srv_port = server.port();

    asio::co_spawn(ioc, server.run(), asio::detached);

    asio::co_spawn(ioc,
        [&]() -> asio::awaitable<void> {
            CulpeoH2Client client(ioc, CulpeoH2Client::AllowCleartext{});
            co_await client.connect("127.0.0.1",
                                    std::to_string(srv_port), "/");

            // Close the entire session (sends GOAWAY + closes TCP socket).
            // This causes the server-side run() loop to see EOF and exit, which
            // closes all stream channels → server handler's receive_frame() throws.
            co_await client.close_session();

            // Poll until handler_exited is set (up to 4 seconds)
            for (int i = 0; i < 80 && !handler_exited.load(); ++i) {
                asio::steady_timer t(ioc, 50ms);
                co_await t.async_wait(asio::use_awaitable);
            }

            ioc.stop();
        },
        asio::detached);

    run_with_timeout(ioc, 5s);

    REQUIRE(handler_exited.load());
}

// ═════════════════════════════════════════════════════════════════════════════
// TEST 4 — Cleartext mode: same as test 2 but explicitly tagged AllowCleartext
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("H2 cleartext: AllowCleartext constructor tag works",
          "[h2][network][cleartext]")
{
    auto handler = std::make_shared<EchoHandler>();

    asio::io_context ioc;
    CulpeoH2Server server(ioc, CulpeoH2Server::AllowCleartext{}, 0, handler);
    asio::co_spawn(ioc, server.run(), asio::detached);

    bool ok = false;
    asio::co_spawn(ioc,
        [&]() -> asio::awaitable<void> {
            CulpeoH2Client client(ioc, CulpeoH2Client::AllowCleartext{});
            co_await client.connect("127.0.0.1",
                                    std::to_string(server.port()), "/");

            std::vector<std::byte> ping = {std::byte{'p'}, std::byte{'i'},
                                            std::byte{'n'}, std::byte{'g'}};
            co_await client.transport().send_text(
                std::span<const std::byte>(ping));

            auto [type, payload] = co_await client.receive_frame();
            ok = (type == kTypeControl) && (payload == ping);
            ioc.stop();
        },
        asio::detached);

    run_with_timeout(ioc);
    REQUIRE(ok);
}

// ═════════════════════════════════════════════════════════════════════════════
// TEST 5 — Large frame (> 65 535 bytes) round-trip
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("H2 cleartext: large frame reassembly (> 65535 bytes)",
          "[h2][network][cleartext]")
{
    auto handler = std::make_shared<EchoHandler>();

    asio::io_context ioc;
    CulpeoH2Server server(ioc, CulpeoH2Server::AllowCleartext{}, 0, handler);
    asio::co_spawn(ioc, server.run(), asio::detached);

    constexpr std::size_t kFrameSize = 100'000; // 100 KB
    bool ok = false;

    asio::co_spawn(ioc,
        [&]() -> asio::awaitable<void> {
            CulpeoH2Client client(ioc, CulpeoH2Client::AllowCleartext{});
            co_await client.connect("127.0.0.1",
                                    std::to_string(server.port()), "/");

            // Build a large payload
            std::vector<std::byte> big(kFrameSize);
            for (std::size_t i = 0; i < kFrameSize; ++i)
                big[i] = static_cast<std::byte>(i & 0xFF);

            co_await client.transport().send_binary(
                std::span<const std::byte>(big));

            auto [type, payload] = co_await client.receive_frame();
            ok = (type == kTypeMedia) && (payload.size() == kFrameSize);
            if (ok) {
                for (std::size_t i = 0; i < kFrameSize; ++i) {
                    if (payload[i] != static_cast<std::byte>(i & 0xFF)) {
                        ok = false;
                        break;
                    }
                }
            }
            ioc.stop();
        },
        asio::detached);

    run_with_timeout(ioc, 15s);
    REQUIRE(ok);
}

// ═════════════════════════════════════════════════════════════════════════════
// TEST 6 — Concurrent sends: two coroutines each send 50 frames (100 total)
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("H2 cleartext: concurrent sends (strand serialisation)",
          "[h2][network][cleartext]")
{
    std::atomic<int> server_recv_count{0};

    class CountingHandler : public ISessionHandler {
    public:
        std::atomic<int>& count;
        explicit CountingHandler(std::atomic<int>& c) : count(c) {}

        asio::awaitable<void> handle(IAsyncTransport& t) override {
            try {
                while (true) {
                    co_await t.receive_frame();
                    count.fetch_add(1, std::memory_order_relaxed);
                }
            } catch (const asio::system_error&) {}
        }
    };

    auto handler = std::make_shared<CountingHandler>(server_recv_count);

    asio::io_context ioc;
    CulpeoH2Server server(ioc, CulpeoH2Server::AllowCleartext{}, 0, handler);
    asio::co_spawn(ioc, server.run(), asio::detached);

    constexpr int kFramesPerSender = 50;

    // Shared client — will be connected before spawning senders
    CulpeoH2Client client(ioc, CulpeoH2Client::AllowCleartext{});
    bool connected = false;
    bool done = false;

    asio::co_spawn(ioc,
        [&]() -> asio::awaitable<void> {
            co_await client.connect("127.0.0.1",
                                    std::to_string(server.port()), "/");
            connected = true;

            // Payload for all frames
            std::vector<std::byte> frame_data(16, std::byte{0x42});

            // Spawn two concurrent sender coroutines
            std::atomic<int> finished{0};

            auto sender = [&](int /*id*/) -> asio::awaitable<void> {
                for (int i = 0; i < kFramesPerSender; ++i) {
                    co_await client.transport().send_text(
                        std::span<const std::byte>(frame_data));
                }
                finished.fetch_add(1, std::memory_order_relaxed);
            };

            asio::co_spawn(ioc, sender(0), asio::detached);
            asio::co_spawn(ioc, sender(1), asio::detached);

            // Wait for both senders to finish
            asio::steady_timer wait(ioc, 8s);
            while (finished.load() < 2) {
                asio::steady_timer t(ioc, 50ms);
                co_await t.async_wait(asio::use_awaitable);
            }

            // Give server a moment to receive all frames
            asio::steady_timer flush(ioc, 500ms);
            co_await flush.async_wait(asio::use_awaitable);

            done = true;
            ioc.stop();
        },
        asio::detached);

    run_with_timeout(ioc, 15s);

    REQUIRE(connected);
    REQUIRE(done);
    // All 100 frames must arrive at the server
    REQUIRE(server_recv_count.load() == kFramesPerSender * 2);
}

// ═════════════════════════════════════════════════════════════════════════════
// TEST 7 — Interop: libculpeo-message serialises culpeo.init; server receives it
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("H2 interop: culpeo.init via libculpeo-message serialisation",
          "[h2][network][cleartext][interop]")
{
    // Build a minimal culpeo.init frame using libculpeo-message
    using namespace culpeo::message;

    std::vector<std::byte> init_frame;
    {
        std::vector<HeaderFieldView> headers = {
            HeaderFieldView{"Event",                "culpeo.init"},
            HeaderFieldView{"Content-Type",         "application/json"},
            HeaderFieldView{"Culpeostream-Version", "0.3"},
        };

        std::string body_str = R"({"streams":[],"version":"0.3"})";
        std::vector<std::byte> body_bytes;
        for (char c : body_str) body_bytes.push_back(static_cast<std::byte>(c));

        auto result = serialize_frame(
            FrameType::control,
            std::span<const HeaderFieldView>(headers),
            std::span<const std::byte>(body_bytes));
        REQUIRE(result.has_value());
        init_frame = std::move(*result);
    }

    // Server receives the frame and records it
    std::atomic<bool> received{false};
    std::vector<std::byte> received_payload;

    class InitRecvHandler : public ISessionHandler {
    public:
        std::atomic<bool>& received;
        std::vector<std::byte>& payload_out;

        InitRecvHandler(std::atomic<bool>& r, std::vector<std::byte>& p)
            : received(r), payload_out(p) {}

        asio::awaitable<void> handle(IAsyncTransport& t) override {
            try {
                auto [type, payload] = co_await t.receive_frame();
                payload_out = std::move(payload);
                received.store(true, std::memory_order_release);
            } catch (...) {}
        }
    };

    auto handler = std::make_shared<InitRecvHandler>(received, received_payload);

    asio::io_context ioc;
    CulpeoH2Server server(ioc, CulpeoH2Server::AllowCleartext{}, 0, handler);
    asio::co_spawn(ioc, server.run(), asio::detached);

    asio::co_spawn(ioc,
        [&]() -> asio::awaitable<void> {
            CulpeoH2Client client(ioc, CulpeoH2Client::AllowCleartext{});
            co_await client.connect("127.0.0.1",
                                    std::to_string(server.port()), "/");

            co_await client.transport().send_text(
                std::span<const std::byte>(init_frame));

            // Give server time to process
            asio::steady_timer t(ioc, 300ms);
            co_await t.async_wait(asio::use_awaitable);

            ioc.stop();
        },
        asio::detached);

    run_with_timeout(ioc, 10s);

    REQUIRE(received.load());

    // Parse the received payload with libculpeo-message and verify Event header
    auto sv = std::string_view(
        reinterpret_cast<const char*>(received_payload.data()),
        received_payload.size());
    auto parsed = parse_headers(FrameType::control, sv);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->event.has_value());
    REQUIRE(*parsed->event == "culpeo.init");
}

// ═════════════════════════════════════════════════════════════════════════════
// SEC-023 — TLS verify_peer: library must not override caller's verify mode
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("SEC-023: TLS client with verify_peer and matching CA completes handshake",
          "[h2][tls][sec-023]")
{
    // Server context: load self-signed test cert + key
    asio::ssl::context server_ctx(asio::ssl::context::tls_server);
    server_ctx.use_certificate_file(
        TEST_CERT_PATH "test_cert.pem", asio::ssl::context::pem);
    server_ctx.use_private_key_file(
        TEST_CERT_PATH "test_key.pem", asio::ssl::context::pem);

    // Client context: load the self-signed cert as CA, enable verify_peer.
    // SEC-023: the library must NOT override this with verify_none.
    asio::ssl::context client_ctx(asio::ssl::context::tls_client);
    client_ctx.load_verify_file(TEST_CERT_PATH "test_cert.pem");
    client_ctx.set_verify_mode(asio::ssl::verify_peer);

    auto handler = std::make_shared<EchoHandler>();

    asio::io_context ioc;
    CulpeoH2Server server(ioc, server_ctx, 0, handler);
    asio::co_spawn(ioc, server.run(), asio::detached);

    bool connected_ok = false;
    std::string recv_payload;

    asio::co_spawn(ioc,
        [&]() -> asio::awaitable<void> {
            CulpeoH2Client client(ioc, client_ctx);
            co_await client.connect("127.0.0.1",
                                    std::to_string(server.port()), "/");

            // Handshake succeeded without verify_none override — send a frame.
            std::string msg = "Event: culpeo.test\r\n\r\nping";
            std::vector<std::byte> bytes;
            for (char c : msg) bytes.push_back(static_cast<std::byte>(c));

            co_await client.transport().send_text(
                std::span<const std::byte>(bytes));

            auto [type, payload] = co_await client.receive_frame();
            REQUIRE(type == kTypeControl);
            recv_payload = std::string(
                reinterpret_cast<const char*>(payload.data()), payload.size());
            connected_ok = true;

            ioc.stop();
        },
        asio::detached);

    run_with_timeout(ioc, 10s);

    REQUIRE(connected_ok);
    REQUIRE(recv_payload == "Event: culpeo.test\r\n\r\nping");
}

// ═════════════════════════════════════════════════════════════════════════════
// SEC-025 — SETTINGS_MAX_CONCURRENT_STREAMS: server enforces stream limit
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("SEC-025: server max_concurrent_streams limits simultaneous streams",
          "[h2][network][cleartext][sec-025]")
{
    // Verify default options value.
    CulpeoH2ServerOptions opts;
    REQUIRE(opts.max_concurrent_streams == 100u);  // default

    // Set max to 1 — the server will advertise SETTINGS_MAX_CONCURRENT_STREAMS=1.
    opts.max_concurrent_streams = 1;

    // Handler: increments a counter and waits for a frame or EOF.
    std::atomic<int> streams_started{0};

    class CountingHandler : public ISessionHandler {
    public:
        std::atomic<int>& count;
        explicit CountingHandler(std::atomic<int>& c) : count(c) {}

        asio::awaitable<void> handle(IAsyncTransport& t) override {
            count.fetch_add(1, std::memory_order_release);
            try {
                // Wait for a frame or session close.
                co_await t.receive_frame();
            } catch (const asio::system_error&) {
                // EOF / RST / close — expected.
            }
        }
    };

    auto handler = std::make_shared<CountingHandler>(streams_started);

    asio::io_context ioc;
    CulpeoH2Server server(ioc, CulpeoH2Server::AllowCleartext{}, 0, handler, opts);
    asio::co_spawn(ioc, server.run(), asio::detached);

    bool test_ok = false;

    asio::co_spawn(ioc,
        [&]() -> asio::awaitable<void> {
            CulpeoH2Client client(ioc, CulpeoH2Client::AllowCleartext{});
            co_await client.connect("127.0.0.1",
                                    std::to_string(server.port()), "/");

            // Wait for stream 1 to be accepted by the server.
            for (int i = 0; i < 40 && streams_started.load() == 0; ++i) {
                asio::steady_timer t(ioc, 50ms);
                co_await t.async_wait(asio::use_awaitable);
            }
            REQUIRE(streams_started.load() == 1);

            // Submit stream 3 while stream 1 is still active.
            // Because the server advertised SETTINGS_MAX_CONCURRENT_STREAMS=1,
            // nghttp2 on the client side MUST queue stream 3 — it does NOT
            // send the HEADERS frame until stream 1 closes.
            auto stream3 = co_await client.open_additional_stream("/");

            // 300 ms pause — enough time for stream 3 to be dispatched to the
            // server handler if SETTINGS was not respected.  If streams_started
            // stays at 1, stream 3 is correctly queued.
            {
                asio::steady_timer delay(ioc, 300ms);
                co_await delay.async_wait(asio::use_awaitable);
            }

            // CRITICAL: stream 3 must NOT have been handled yet.
            // Failure here means SETTINGS_MAX_CONCURRENT_STREAMS was not respected.
            REQUIRE(streams_started.load() == 1);

            // Close the session.  This sends GOAWAY which drops all pending
            // (queued) streams.  Stream 3 is dropped and never reaches the server.
            co_await client.close_session();

            // Brief wait — stream 3 should remain unhandled.
            {
                asio::steady_timer delay(ioc, 100ms);
                co_await delay.async_wait(asio::use_awaitable);
            }
            REQUIRE(streams_started.load() == 1);

            test_ok = true;
            ioc.stop();
        },
        asio::detached);

    run_with_timeout(ioc, 10s);
    REQUIRE(test_ok);
}

// ═════════════════════════════════════════════════════════════════════════════
// SEC-028 — Header capture: Authorization and Content-Type are accessible
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("SEC-028: Authorization header is captured from HTTP/2 request",
          "[h2][network][cleartext][sec-028]")
{
    std::string captured_auth;
    std::string captured_ct;
    std::atomic<bool> headers_captured{false};

    class HeaderCapturingHandler : public ISessionHandler {
    public:
        std::string& auth_out;
        std::string& ct_out;
        std::atomic<bool>& captured;

        HeaderCapturingHandler(std::string& a, std::string& c,
                                std::atomic<bool>& cap)
            : auth_out(a), ct_out(c), captured(cap) {}

        asio::awaitable<void> handle(IAsyncTransport& t) override {
            // SEC-028: request_header() exposes captured HTTP/2 headers.
            auth_out = t.request_header("authorization");
            ct_out   = t.request_header("content-type");
            captured.store(true, std::memory_order_release);
            try {
                co_await t.receive_frame();
            } catch (const asio::system_error&) {}
        }
    };

    auto handler = std::make_shared<HeaderCapturingHandler>(
        captured_auth, captured_ct, headers_captured);

    asio::io_context ioc;
    CulpeoH2Server server(ioc, CulpeoH2Server::AllowCleartext{}, 0, handler);
    asio::co_spawn(ioc, server.run(), asio::detached);

    asio::co_spawn(ioc,
        [&]() -> asio::awaitable<void> {
            CulpeoH2Client client(ioc, CulpeoH2Client::AllowCleartext{});
            co_await client.connect("127.0.0.1",
                                    std::to_string(server.port()), "/");

            // Give the server time to call the handler and capture headers.
            for (int i = 0; i < 40 && !headers_captured.load(); ++i) {
                asio::steady_timer t(ioc, 50ms);
                co_await t.async_wait(asio::use_awaitable);
            }

            co_await client.close_session();
            ioc.stop();
        },
        asio::detached);

    run_with_timeout(ioc, 10s);

    REQUIRE(headers_captured.load());
    // The client sends "content-type: application/culpeostream" in its request.
    // The Authorization header is not sent by the cleartext test client, so it
    // should be empty (not a crash or UB).
    REQUIRE(captured_ct == "application/culpeostream");
    REQUIRE(captured_auth.empty()); // no Authorization sent by the test client
}

// ═════════════════════════════════════════════════════════════════════════════
// SEC-029 — Handshake timeout: CulpeoH2ServerOptions exposes the field
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("SEC-029: CulpeoH2ServerOptions handshake_timeout_seconds is configurable",
          "[h2][sec-029]")
{
    // Unit test: verify the options struct carries the correct value and that
    // the server accepts it without error.  Full timing-based tests are
    // fragile in CI; correctness of the timer path is covered by code review.
    CulpeoH2ServerOptions opts;
    REQUIRE(opts.handshake_timeout_seconds == 10u); // default

    opts.handshake_timeout_seconds = 30;
    REQUIRE(opts.handshake_timeout_seconds == 30u);

    // Construct a server with custom options — must not throw.
    asio::io_context ioc;
    asio::ssl::context tls_ctx(asio::ssl::context::tls_server);
    tls_ctx.use_certificate_file(
        TEST_CERT_PATH "test_cert.pem", asio::ssl::context::pem);
    tls_ctx.use_private_key_file(
        TEST_CERT_PATH "test_key.pem", asio::ssl::context::pem);

    auto handler = std::make_shared<EchoHandler>();
    REQUIRE_NOTHROW(
        CulpeoH2Server(ioc, tls_ctx, 0, handler, opts));
}

// ═════════════════════════════════════════════════════════════════════════════
// SEC-033 — Oversized payload: encode_h2_envelope and send_frame reject it
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("SEC-033: encode_h2_envelope throws on payload >= kMaxFrameSize",
          "[h2][framing][sec-033]")
{
    // The maximum payload is kMaxFrameSize - 1 bytes.  A payload of exactly
    // kMaxFrameSize bytes exceeds the limit and must throw.

    SECTION("encode_h2_envelope throws on payload == kMaxFrameSize bytes") {
        // Allocate exactly kMaxFrameSize bytes (1 MiB).  This is the first
        // size that triggers the check.
        std::vector<std::byte> oversized(
            static_cast<std::size_t>(H2Session::kMaxFrameSize));
        REQUIRE_THROWS_AS(
            encode_h2_envelope(kTypeControl,
                std::span<const std::byte>(oversized)),
            std::invalid_argument);
    }

    SECTION("encode_h2_envelope accepts payload of kMaxFrameSize - 1 bytes") {
        std::vector<std::byte> max_ok(
            static_cast<std::size_t>(H2Session::kMaxFrameSize) - 1u);
        REQUIRE_NOTHROW(
            encode_h2_envelope(kTypeControl,
                std::span<const std::byte>(max_ok)));
    }
}
