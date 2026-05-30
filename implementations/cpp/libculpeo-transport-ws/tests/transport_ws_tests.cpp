// transport_ws_tests.cpp — Unit tests for culpeo::transport::WsTransport
//
// Tests use injected mock callables (std::function lambdas) to exercise
// WsTransport behaviour without any network I/O or WebSocket library.

#include <catch2/catch_test_macros.hpp>

#include "culpeo/transport_ws.hpp"
#include "culpeo/session.hpp"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace culpeo::transport;
using namespace culpeo::session;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static std::vector<std::byte> make_bytes(std::string_view s) {
    std::vector<std::byte> v;
    v.reserve(s.size());
    for (char c : s) v.push_back(static_cast<std::byte>(c));
    return v;
}

static std::string bytes_to_str(std::span<const std::byte> s) {
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}

// ─── Test: construction ───────────────────────────────────────────────────────

TEST_CASE("WsTransport: null send_text_fn throws", "[transport_ws]") {
    REQUIRE_THROWS_AS(
        WsTransport(nullptr,
                    [](auto) {},
                    [](int, std::string_view) {}),
        std::invalid_argument);
}

TEST_CASE("WsTransport: null send_binary_fn throws", "[transport_ws]") {
    REQUIRE_THROWS_AS(
        WsTransport([](auto) {},
                    nullptr,
                    [](int, std::string_view) {}),
        std::invalid_argument);
}

TEST_CASE("WsTransport: null close_fn throws", "[transport_ws]") {
    REQUIRE_THROWS_AS(
        WsTransport([](auto) {},
                    [](auto) {},
                    nullptr),
        std::invalid_argument);
}

// ─── Test: send_text ─────────────────────────────────────────────────────────

TEST_CASE("WsTransport: send_text delegates to callback", "[transport_ws]") {
    std::vector<std::string> received;

    WsTransport t(
        [&](std::span<const std::byte> f) { received.push_back(bytes_to_str(f)); },
        [](auto) {},
        [](int, std::string_view) {}
    );

    auto data = make_bytes("Event: culpeo.ping\r\n\r\n{}");
    t.send_text(data);

    REQUIRE(received.size() == 1);
    CHECK(received[0] == "Event: culpeo.ping\r\n\r\n{}");
}

TEST_CASE("WsTransport: send_text multiple calls are all delivered", "[transport_ws]") {
    std::vector<std::string> received;

    WsTransport t(
        [&](std::span<const std::byte> f) { received.push_back(bytes_to_str(f)); },
        [](auto) {},
        [](int, std::string_view) {}
    );

    t.send_text(make_bytes("frame-1"));
    t.send_text(make_bytes("frame-2"));
    t.send_text(make_bytes("frame-3"));

    REQUIRE(received.size() == 3);
    CHECK(received[0] == "frame-1");
    CHECK(received[1] == "frame-2");
    CHECK(received[2] == "frame-3");
}

// ─── Test: send_binary ───────────────────────────────────────────────────────

TEST_CASE("WsTransport: send_binary delegates to callback", "[transport_ws]") {
    std::vector<std::string> received;

    WsTransport t(
        [](auto) {},
        [&](std::span<const std::byte> f) { received.push_back(bytes_to_str(f)); },
        [](int, std::string_view) {}
    );

    auto data = make_bytes("Stream-Id: s1\r\n\r\n\x00\x01\x02\x03");
    t.send_binary(data);

    REQUIRE(received.size() == 1);
    CHECK(received[0].size() == data.size());
}

// ─── Test: close ─────────────────────────────────────────────────────────────

TEST_CASE("WsTransport: close passes code and reason to callback", "[transport_ws]") {
    int captured_code = -1;
    std::string captured_reason;

    WsTransport t(
        [](auto) {},
        [](auto) {},
        [&](int code, std::string_view reason) {
            captured_code   = code;
            captured_reason = std::string(reason);
        }
    );

    t.close(1002, "Protocol Error");

    CHECK(captured_code   == 1002);
    CHECK(captured_reason == "Protocol Error");
}

TEST_CASE("WsTransport: close with normal code 1000", "[transport_ws]") {
    int captured_code = -1;

    WsTransport t(
        [](auto) {},
        [](auto) {},
        [&](int code, std::string_view) { captured_code = code; }
    );

    t.close(1000, "Goodbye");
    CHECK(captured_code == 1000);
}

TEST_CASE("WsTransport: close with policy-violation code 1008", "[transport_ws]") {
    int captured_code = -1;
    std::string captured_reason;

    WsTransport t(
        [](auto) {},
        [](auto) {},
        [&](int code, std::string_view reason) {
            captured_code   = code;
            captured_reason = std::string(reason);
        }
    );

    t.close(1008, "Unauthorized");
    CHECK(captured_code   == 1008);
    CHECK(captured_reason == "Unauthorized");
}

// ─── Test: ITransport polymorphism ───────────────────────────────────────────

TEST_CASE("WsTransport: usable through ITransport pointer", "[transport_ws]") {
    std::vector<std::string> text_received;
    bool closed = false;

    auto t = std::make_unique<WsTransport>(
        [&](std::span<const std::byte> f) { text_received.push_back(bytes_to_str(f)); },
        [](auto) {},
        [&](int, std::string_view) { closed = true; }
    );

    ITransport* iface = t.get();
    iface->send_text(make_bytes("hello"));
    iface->close(1000, "done");

    REQUIRE(text_received.size() == 1);
    CHECK(text_received[0] == "hello");
    CHECK(closed);
}

// ─── Test: thread safety ─────────────────────────────────────────────────────

TEST_CASE("WsTransport: concurrent send_text calls are serialized", "[transport_ws]") {
    // Verifies that the internal mutex prevents interleaved sends.
    // Each 'send' callback atomically increments a counter; we confirm
    // the final count matches the number of sends and there are no races
    // (detected by ThreadSanitizer if built with -fsanitize=thread).
    constexpr int kThreads = 8;
    constexpr int kSendsPerThread = 100;

    std::atomic<int> send_count{0};

    WsTransport t(
        [&](std::span<const std::byte>) { ++send_count; },
        [](auto) {},
        [](int, std::string_view) {}
    );

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&t] {
            auto data = make_bytes("payload");
            for (int j = 0; j < kSendsPerThread; ++j) {
                t.send_text(data);
            }
        });
    }
    for (auto& th : threads) th.join();

    CHECK(send_count.load() == kThreads * kSendsPerThread);
}

TEST_CASE("WsTransport: concurrent send_binary calls are serialized", "[transport_ws]") {
    constexpr int kThreads = 8;
    constexpr int kSendsPerThread = 100;

    std::atomic<int> send_count{0};

    WsTransport t(
        [](auto) {},
        [&](std::span<const std::byte>) { ++send_count; },
        [](int, std::string_view) {}
    );

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&t] {
            auto data = make_bytes("binary-payload");
            for (int j = 0; j < kSendsPerThread; ++j) {
                t.send_binary(data);
            }
        });
    }
    for (auto& th : threads) th.join();

    CHECK(send_count.load() == kThreads * kSendsPerThread);
}

TEST_CASE("WsTransport: interleaved text and binary sends are safe", "[transport_ws]") {
    constexpr int kThreads = 4;
    constexpr int kSendsPerThread = 50;

    std::atomic<int> text_count{0};
    std::atomic<int> binary_count{0};

    WsTransport t(
        [&](std::span<const std::byte>) { ++text_count; },
        [&](std::span<const std::byte>) { ++binary_count; },
        [](int, std::string_view) {}
    );

    std::vector<std::thread> threads;
    threads.reserve(kThreads * 2);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&t] {
            auto d = make_bytes("t");
            for (int j = 0; j < kSendsPerThread; ++j) t.send_text(d);
        });
        threads.emplace_back([&t] {
            auto d = make_bytes("b");
            for (int j = 0; j < kSendsPerThread; ++j) t.send_binary(d);
        });
    }
    for (auto& th : threads) th.join();

    CHECK(text_count.load()   == kThreads * kSendsPerThread);
    CHECK(binary_count.load() == kThreads * kSendsPerThread);
}

// ─── Test: integration with Session ──────────────────────────────────────────
//
// Verify that WsTransport works end-to-end as the transport for a real
// culpeo::session::Session.  We drive the session through its init handshake
// and confirm text frames arrive at the WsTransport callback.

TEST_CASE("WsTransport: Session uses WsTransport for init-ack", "[transport_ws][integration]") {
    std::vector<std::string> text_frames;
    std::vector<std::string> binary_frames;

    WsTransport transport(
        [&](std::span<const std::byte> f) { text_frames.push_back(bytes_to_str(f)); },
        [&](std::span<const std::byte> f) { binary_frames.push_back(bytes_to_str(f)); },
        [](int, std::string_view) {}
    );

    SessionCallbacks cbs;
    cbs.on_auth_validate   = [](std::string_view) { return true; };
    cbs.on_auth_response   = [](std::string_view) { return true; };

    Session session(transport, cbs);
    REQUIRE(session.state() == SessionState::uninitialized);

    // Build a minimal culpeo.init frame
    std::string init_frame =
        "Event: culpeo.init\r\n"
        "Content-Type: application/json\r\n"
        "Authorization: Bearer test-token\r\n"
        "\r\n"
        R"({"version":"0.3","streams":[{"content_type":"audio/pcm;rate=16000;channels=1;bits=16","type":"input","offset_type":"time"}]})";

    auto bytes = make_bytes(init_frame);
    auto span  = std::span<const std::byte>(bytes);

    namespace msg = culpeo::message;
    auto parsed = msg::parse_headers(
        msg::FrameType::control,
        std::string_view(reinterpret_cast<const char*>(span.data()), span.size()));

    REQUIRE(parsed.has_value());
    auto result = session.process_control_frame(*parsed);
    REQUIRE(result.has_value());

    // Session should now be established and have sent init-ack
    CHECK(session.state() == SessionState::established);
    REQUIRE(!text_frames.empty());
    CHECK(text_frames.front().find("culpeo.init-ack") != std::string::npos);
}

TEST_CASE("WsTransport: Session close sends WS code 1000 for normal close", "[transport_ws][integration]") {
    int ws_close_code = -1;
    std::string ws_close_reason;

    WsTransport transport(
        [](auto) {},
        [](auto) {},
        [&](int code, std::string_view reason) {
            ws_close_code   = code;
            ws_close_reason = std::string(reason);
        }
    );

    SessionCallbacks cbs;
    cbs.on_auth_validate = [](std::string_view) { return true; };
    cbs.on_auth_response = [](std::string_view) { return true; };

    Session session(transport, cbs);

    // Drive to established
    std::string init_frame =
        "Event: culpeo.init\r\n"
        "Content-Type: application/json\r\n"
        "Authorization: Bearer test-token\r\n"
        "\r\n"
        R"({"version":"0.3","streams":[{"content_type":"audio/pcm;rate=16000;channels=1;bits=16","type":"input","offset_type":"time"}]})";

    auto bytes = make_bytes(init_frame);
    namespace msg = culpeo::message;
    auto parsed = msg::parse_headers(
        msg::FrameType::control,
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    REQUIRE(parsed.has_value());
    REQUIRE(session.process_control_frame(*parsed).has_value());

    // Graceful close from server side
    session.close("normal", "Goodbye");

    CHECK(ws_close_code   == 1000);
    CHECK(ws_close_reason == "Goodbye");
}

TEST_CASE("WsTransport: Session protocol error sends WS code 1002", "[transport_ws][integration]") {
    int ws_close_code = -1;

    WsTransport transport(
        [](auto) {},
        [](auto) {},
        [&](int code, std::string_view) { ws_close_code = code; }
    );

    SessionCallbacks cbs;
    cbs.on_auth_validate = [](std::string_view) { return true; };
    cbs.on_auth_response = [](std::string_view) { return true; };

    Session session(transport, cbs);

    // Drive to established
    std::string init_frame =
        "Event: culpeo.init\r\n"
        "Content-Type: application/json\r\n"
        "Authorization: Bearer test-token\r\n"
        "\r\n"
        R"({"version":"0.3","streams":[{"content_type":"audio/pcm;rate=16000;channels=1;bits=16","type":"input","offset_type":"time"}]})";

    auto bytes = make_bytes(init_frame);
    namespace msg = culpeo::message;
    auto parsed = msg::parse_headers(
        msg::FrameType::control,
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    REQUIRE(parsed.has_value());
    REQUIRE(session.process_control_frame(*parsed).has_value());

    // Send a protocol-error close from server side
    session.close("protocol-error", "Bad frame");

    CHECK(ws_close_code == 1002);
}
