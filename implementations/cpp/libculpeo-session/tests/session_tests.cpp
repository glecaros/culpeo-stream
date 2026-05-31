#include <catch2/catch_test_macros.hpp>

#include "culpeo/session.hpp"
#include "culpeo/message.hpp"

#include <atomic>
#include <chrono>
#include <format>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace culpeo::session;
namespace frame = culpeo::message;

// Disambiguate: both culpeo::session and culpeo::message define Error
using Error = culpeo::session::Error;

// ─── Test infrastructure ─────────────────────────────────────────────────────

struct MockTransport : ITransport {
    mutable std::mutex mu;
    std::vector<std::vector<std::byte>> text_sent;
    std::vector<std::vector<std::byte>> binary_sent;
    bool closed{false};
    int close_code{0};
    std::string close_reason;

    void send_text(std::span<const std::byte> frame) override {
        std::lock_guard<std::mutex> lock(mu);
        text_sent.emplace_back(frame.begin(), frame.end());
    }
    void send_binary(std::span<const std::byte> frame) override {
        std::lock_guard<std::mutex> lock(mu);
        binary_sent.emplace_back(frame.begin(), frame.end());
    }
    void close(int code, std::string_view reason) override {
        std::lock_guard<std::mutex> lock(mu);
        closed = true;
        close_code = code;
        close_reason = std::string(reason);
    }

    std::size_t text_count() const {
        std::lock_guard<std::mutex> lock(mu);
        return text_sent.size();
    }
    std::size_t binary_count() const {
        std::lock_guard<std::mutex> lock(mu);
        return binary_sent.size();
    }

    // Get the Nth text frame as a string
    std::string text_frame_str(std::size_t n) const {
        std::lock_guard<std::mutex> lock(mu);
        if (n >= text_sent.size()) return "";
        const auto& f = text_sent[n];
        return std::string(reinterpret_cast<const char*>(f.data()), f.size());
    }

    bool last_text_contains(std::string_view needle) const {
        std::lock_guard<std::mutex> lock(mu);
        if (text_sent.empty()) return false;
        const auto& f = text_sent.back();
        std::string s(reinterpret_cast<const char*>(f.data()), f.size());
        return s.find(needle) != std::string::npos;
    }

    bool any_text_contains(std::string_view needle) const {
        std::lock_guard<std::mutex> lock(mu);
        for (const auto& f : text_sent) {
            std::string s(reinterpret_cast<const char*>(f.data()), f.size());
            if (s.find(needle) != std::string::npos) return true;
        }
        return false;
    }
};

// Build a text (control) frame string from headers + body
static std::string make_ctrl_frame(std::initializer_list<std::pair<std::string_view, std::string_view>> headers,
                                    std::string_view body) {
    std::string frame;
    for (const auto& [k, v] : headers) {
        frame += std::string(k) + ": " + std::string(v) + "\r\n";
    }
    frame += "\r\n";
    frame += body;
    return frame;
}

// Build a binary (media) frame string
static std::string make_media_frame(std::initializer_list<std::pair<std::string_view, std::string_view>> headers,
                                     std::string_view body = "") {
    return make_ctrl_frame(headers, body);
}

// Parse and feed a text frame to the session
static std::expected<void, Error>
feed_text(Session& session, const std::string& raw) {
    auto parsed = frame::parse_headers(frame::FrameType::control, raw);
    REQUIRE(parsed.has_value());
    return session.process_control_frame(*parsed);
}

// Parse and feed a binary frame to the session
static std::expected<void, Error>
feed_binary(Session& session, const std::string& raw) {
    auto parsed = frame::parse_headers(frame::FrameType::media, raw);
    REQUIRE(parsed.has_value());
    return session.process_media_frame(*parsed);
}

// A valid culpeo.init body with one input stream (fresh session)
static std::string basic_init_body(std::string_view version = "0.3") {
    return std::string(R"({"version":")") + std::string(version) + R"(","streams":[{"content_type":"audio/pcm;rate=16000;channels=1;bits=16","type":"input","offset_type":"time"}]})";
}

// ─── State machine: basic lifecycle ──────────────────────────────────────────

TEST_CASE("Session starts in uninitialized state", "[session]") {
    MockTransport transport;
    Session session(transport);
    CHECK(session.state() == SessionState::uninitialized);
    CHECK_FALSE(session.session_id().has_value());
    CHECK(session.streams().empty());
}

TEST_CASE("Fresh session: culpeo.init → established", "[session]") {
    MockTransport transport;
    SessionCallbacks cbs;
    cbs.on_auth_validate = [](std::string_view) { return true; };

    Session session(transport, std::move(cbs));

    auto raw = make_ctrl_frame(
        {{"Event", "culpeo.init"},
         {"Authorization", "Bearer test-token"},
         {"Content-Type", "application/json"}},
        basic_init_body());

    auto result = feed_text(session, raw);
    REQUIRE(result.has_value());

    CHECK(session.state() == SessionState::established);
    REQUIRE(session.session_id().has_value());
    CHECK(session.session_id()->size() == 32);  // 128-bit hex
    CHECK(session.streams().size() == 1);

    // Should have sent culpeo.init-ack
    REQUIRE(transport.text_count() == 1);
    CHECK(transport.any_text_contains("culpeo.init-ack"));
    CHECK(transport.any_text_contains("Session-Id"));
}

TEST_CASE("Fresh session: auth validation called with Bearer token", "[session]") {
    MockTransport transport;
    std::string captured_token;
    SessionCallbacks cbs;
    cbs.on_auth_validate = [&](std::string_view token) {
        captured_token = token;
        return true;
    };

    Session session(transport, std::move(cbs));

    auto raw = make_ctrl_frame(
        {{"Event", "culpeo.init"},
         {"Authorization", "Bearer my-secret-token"},
         {"Content-Type", "application/json"}},
        basic_init_body());

    feed_text(session, raw);
    CHECK(captured_token == "my-secret-token");
}

TEST_CASE("Fresh session: auth failure → init-error sent, session closed", "[session]") {
    MockTransport transport;
    SessionCallbacks cbs;
    cbs.on_auth_validate = [](std::string_view) { return false; };

    Session session(transport, std::move(cbs));

    auto raw = make_ctrl_frame(
        {{"Event", "culpeo.init"},
         {"Authorization", "Bearer bad-token"},
         {"Content-Type", "application/json"}},
        basic_init_body());

    auto result = feed_text(session, raw);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::auth_failed);
    CHECK(session.state() == SessionState::closed);
    CHECK(transport.any_text_contains("culpeo.init-error"));
    CHECK(transport.any_text_contains("unauthorized"));
    CHECK(transport.closed);
}

TEST_CASE("Fresh session: unsupported version → init-error with supported_versions", "[session]") {
    MockTransport transport;
    Session session(transport);

    auto raw = make_ctrl_frame(
        {{"Event", "culpeo.init"},
         {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}},
        R"({"version":"0.2","streams":[{"content_type":"audio/opus","type":"input","offset_type":"message"}]})");

    auto result = feed_text(session, raw);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::version_unsupported);
    CHECK(transport.any_text_contains("unsupported-version"));
    CHECK(transport.any_text_contains("supported_versions"));
    CHECK(transport.closed);
}

TEST_CASE("Fresh session: invalid stream declarations → init-error", "[session]") {
    MockTransport transport;
    Session session(transport);

    // Two input streams without purpose (violates §5.5 rule 4)
    auto raw = make_ctrl_frame(
        {{"Event", "culpeo.init"},
         {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}},
        R"({"version":"0.3","streams":[
            {"content_type":"audio/pcm;rate=16000;channels=1;bits=16","type":"input","offset_type":"time"},
            {"content_type":"audio/pcm;rate=44100;channels=2;bits=16","type":"input","offset_type":"time"}
        ]})");

    auto result = feed_text(session, raw);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::invalid_streams);
    CHECK(transport.any_text_contains("culpeo.init-error"));
    CHECK(transport.any_text_contains("invalid-streams"));
}

TEST_CASE("Fresh session: missing streams array → json_error", "[session]") {
    MockTransport transport;
    Session session(transport);

    auto raw = make_ctrl_frame(
        {{"Event", "culpeo.init"},
         {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}},
        R"({"version":"0.3"})");  // No "streams" key

    auto result = feed_text(session, raw);
    REQUIRE_FALSE(result.has_value());
    // Either json_error or invalid_streams
    CHECK((result.error() == Error::json_error || result.error() == Error::invalid_streams));
    CHECK(transport.closed);
}

// ─── Stream stream-count limits ───────────────────────────────────────────────

TEST_CASE("Too many streams → max_streams_exceeded", "[session]") {
    MockTransport transport;
    SessionConfig cfg;
    cfg.max_streams = 2;
    Session session(transport, {}, cfg);

    // 3 streams where max is 2
    auto raw = make_ctrl_frame(
        {{"Event", "culpeo.init"},
         {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}},
        R"({"version":"0.3","streams":[
            {"content_type":"audio/pcm;rate=16000;channels=1;bits=16","type":"input","purpose":"a","offset_type":"time"},
            {"content_type":"audio/opus","type":"output","purpose":"b","offset_type":"message"},
            {"content_type":"application/json","type":"duplex","purpose":"c","offset_type":"message"}
        ]})");

    auto result = feed_text(session, raw);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::max_streams_exceeded);
}

// ─── Buffer-Window negotiation ────────────────────────────────────────────────

TEST_CASE("Buffer-Window is clamped to server max", "[session]") {
    MockTransport transport;
    SessionConfig cfg;
    cfg.max_buffer_window_ms = 5000;

    Session session(transport, {}, cfg);

    auto raw = make_ctrl_frame(
        {{"Event", "culpeo.init"},
         {"Authorization", "Bearer token"},
         {"Buffer-Window", "60000"},
         {"Content-Type", "application/json"}},
        basic_init_body());

    feed_text(session, raw);
    // init-ack should contain Buffer-Window ≤ 5000
    REQUIRE(transport.text_count() >= 1);
    const auto init_ack = transport.text_frame_str(0);
    // Find "Buffer-Window: " in the frame
    auto pos = init_ack.find("Buffer-Window: ");
    REQUIRE(pos != std::string::npos);
    const auto bw_start = pos + 15;
    const auto bw_end = init_ack.find("\r\n", bw_start);
    const auto bw_str = init_ack.substr(bw_start, bw_end - bw_start);
    CHECK(std::stoul(bw_str) <= 5000);
}

// ─── Media frames ─────────────────────────────────────────────────────────────

TEST_CASE("Media frame on input stream: accepted and offset advances", "[session]") {
    MockTransport transport;
    SessionCallbacks cbs;
    cbs.on_auth_validate = [](std::string_view) { return true; };

    bool media_received = false;
    uint64_t received_ts = 0;
    cbs.on_media_received = [&](const StreamInfo&, uint64_t ts, std::span<const std::byte>) {
        media_received = true;
        received_ts = ts;
    };

    Session session(transport, std::move(cbs));

    // Establish session
    feed_text(session, make_ctrl_frame(
        {{"Event", "culpeo.init"},
         {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}},
        basic_init_body()));

    REQUIRE(session.state() == SessionState::established);
    auto streams = session.streams();
    REQUIRE(streams.size() == 1);
    const std::string stream_id = streams[0].id;

    // Send 16 bytes of PCM at offset 0 → 8 samples (16-bit mono)
    const std::string pcm_payload(16, '\x7f');
    auto raw = make_media_frame(
        {{"Stream-Id", stream_id},
         {"Offset", "0"},
         {"Content-Type", "audio/pcm;rate=16000;channels=1;bits=16"},
         {"Timestamp", "1000"}},
        pcm_payload);

    auto result = feed_binary(session, raw);
    REQUIRE(result.has_value());
    CHECK(media_received);
    CHECK(received_ts == 1000);

    // Offset should have advanced by 8 samples
    auto updated_streams = session.streams();
    CHECK(updated_streams[0].offset == 8);
}

TEST_CASE("Media frame with wrong offset → protocol error, session closed", "[session]") {
    MockTransport transport;
    SessionCallbacks cbs;
    cbs.on_auth_validate = [](std::string_view) { return true; };
    Session session(transport, std::move(cbs));

    feed_text(session, make_ctrl_frame(
        {{"Event", "culpeo.init"},
         {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}},
        basic_init_body()));

    auto streams = session.streams();
    const std::string stream_id = streams[0].id;

    // Correct offset is 0 but we send 100
    auto raw = make_media_frame(
        {{"Stream-Id", stream_id},
         {"Offset", "100"},
         {"Content-Type", "audio/pcm;rate=16000;channels=1;bits=16"},
         {"Timestamp", "0"}},
        std::string(16, '\0'));

    auto result = feed_binary(session, raw);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::offset_mismatch);
    CHECK(session.state() == SessionState::closed);
    CHECK(transport.any_text_contains("culpeo.close"));
}

TEST_CASE("Media frame on output stream from client → direction error", "[session]") {
    MockTransport transport;
    SessionCallbacks cbs;
    cbs.on_auth_validate = [](std::string_view) { return true; };
    Session session(transport, std::move(cbs));

    // Session with an output stream (server → client)
    auto init_raw = make_ctrl_frame(
        {{"Event", "culpeo.init"},
         {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}},
        R"({"version":"0.3","streams":[{"content_type":"audio/opus","type":"output","offset_type":"message"}]})");

    feed_text(session, init_raw);
    REQUIRE(session.state() == SessionState::established);

    auto streams = session.streams();
    const std::string stream_id = streams[0].id;

    // Client tries to send on an output-only stream
    auto raw = make_media_frame(
        {{"Stream-Id", stream_id},
         {"Offset", "0"},
         {"Content-Type", "audio/opus"},
         {"Timestamp", "0"}},
        std::string(20, '\x00'));

    auto result = feed_binary(session, raw);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::invalid_direction);
    CHECK(session.state() == SessionState::closed);
}

TEST_CASE("Media frame before culpeo.init → protocol error", "[session]") {
    MockTransport transport;
    Session session(transport);

    auto raw = make_media_frame(
        {{"Stream-Id", "s1"},
         {"Offset", "0"},
         {"Content-Type", "audio/opus"},
         {"Timestamp", "0"}},
        std::string(20, '\x00'));

    auto result = feed_binary(session, raw);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::wrong_state);
}

// ─── send_media ───────────────────────────────────────────────────────────────

TEST_CASE("send_media: server sends on output stream", "[session]") {
    MockTransport transport;
    SessionCallbacks cbs;
    cbs.on_auth_validate = [](std::string_view) { return true; };
    Session session(transport, std::move(cbs));

    feed_text(session, make_ctrl_frame(
        {{"Event", "culpeo.init"},
         {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}},
        R"({"version":"0.3","streams":[{"content_type":"audio/opus","type":"output","offset_type":"message"}]})"));

    REQUIRE(session.state() == SessionState::established);
    auto streams = session.streams();
    const std::string sid = streams[0].id;

    const std::string payload(120, '\x42');
    auto result = session.send_media(sid,
        culpeo::message::as_bytes(payload), 50000);
    REQUIRE(result.has_value());

    // One binary frame should have been sent
    CHECK(transport.binary_count() == 1);

    // Offset should have advanced by 1 (Opus)
    auto updated = session.streams();
    CHECK(updated[0].offset == 1);
}

TEST_CASE("send_media: offset in binary frame matches stream offset before advance", "[session]") {
    MockTransport transport;
    SessionCallbacks cbs;
    cbs.on_auth_validate = [](std::string_view) { return true; };
    Session session(transport, std::move(cbs));

    feed_text(session, make_ctrl_frame(
        {{"Event", "culpeo.init"},
         {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}},
        R"({"version":"0.3","streams":[{"content_type":"audio/opus","type":"output","offset_type":"message"}]})"));

    auto streams = session.streams();
    const std::string sid = streams[0].id;

    // First send: offset 0
    session.send_media(sid, culpeo::message::as_bytes(std::string(50, '\0')), 0);
    // Second send: offset 1
    session.send_media(sid, culpeo::message::as_bytes(std::string(50, '\0')), 0);

    // Verify binary frames contain correct offsets
    REQUIRE(transport.binary_count() == 2);
    // Parse the frames and check offsets
    {
        const auto& f0 = transport.binary_sent[0];
        std::string s(reinterpret_cast<const char*>(f0.data()), f0.size());
        CHECK(s.find("Offset: 0") != std::string::npos);
    }
    {
        const auto& f1 = transport.binary_sent[1];
        std::string s(reinterpret_cast<const char*>(f1.data()), f1.size());
        CHECK(s.find("Offset: 1") != std::string::npos);
    }
}

TEST_CASE("send_media: rejects send on input stream", "[session]") {
    MockTransport transport;
    SessionCallbacks cbs;
    cbs.on_auth_validate = [](std::string_view) { return true; };
    Session session(transport, std::move(cbs));

    feed_text(session, make_ctrl_frame(
        {{"Event", "culpeo.init"},
         {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}},
        basic_init_body()));

    auto streams = session.streams();
    const std::string sid = streams[0].id;  // input stream

    auto result = session.send_media(sid,
        culpeo::message::as_bytes(std::string(16, '\0')), 0);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::invalid_direction);
}

TEST_CASE("send_media: rejects send when not established", "[session]") {
    MockTransport transport;
    Session session(transport);

    auto result = session.send_media("s1",
        culpeo::message::as_bytes(std::string(16, '\0')), 0);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::wrong_state);
}

// ─── Ping / Pong ──────────────────────────────────────────────────────────────

TEST_CASE("culpeo.ping → culpeo.pong response", "[session]") {
    MockTransport transport;
    SessionCallbacks cbs;
    cbs.on_auth_validate = [](std::string_view) { return true; };
    Session session(transport, std::move(cbs));

    feed_text(session, make_ctrl_frame(
        {{"Event", "culpeo.init"}, {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}}, basic_init_body()));

    const std::size_t frames_before = transport.text_count();

    auto ping_raw = make_ctrl_frame(
        {{"Event", "culpeo.ping"}, {"Content-Type", "application/json"}},
        R"({"ts":1716393600000000})");

    auto result = feed_text(session, ping_raw);
    REQUIRE(result.has_value());

    REQUIRE(transport.text_count() == frames_before + 1);
    CHECK(transport.any_text_contains("culpeo.pong"));
    CHECK(transport.any_text_contains("1716393600000000"));
    CHECK(transport.any_text_contains("server_ts"));
}

TEST_CASE("culpeo.ping rate limiting: excess pings dropped silently", "[session]") {
    MockTransport transport;
    SessionConfig cfg;
    cfg.max_ping_rate_per_second = 2;
    SessionCallbacks cbs;
    cbs.on_auth_validate = [](std::string_view) { return true; };

    Session session(transport, std::move(cbs), cfg);

    feed_text(session, make_ctrl_frame(
        {{"Event", "culpeo.init"}, {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}}, basic_init_body()));

    // Reset count
    const std::size_t ack_count = transport.text_count();

    auto ping_raw = make_ctrl_frame(
        {{"Event", "culpeo.ping"}, {"Content-Type", "application/json"}},
        R"({"ts":1000})");

    // Send 5 pings quickly — only first 2 should get pong responses
    for (int i = 0; i < 5; ++i) {
        feed_text(session, ping_raw);
    }

    // No more than 2 pong frames should have been sent
    const std::size_t pong_count = transport.text_count() - ack_count;
    CHECK(pong_count <= 2);

    // Session should still be established
    CHECK(session.state() == SessionState::established);
}

TEST_CASE("Server-initiated ping → RTT callback on pong", "[session]") {
    MockTransport transport;
    SessionCallbacks cbs;
    cbs.on_auth_validate = [](std::string_view) { return true; };
    std::optional<std::chrono::microseconds> measured_rtt;
    cbs.on_rtt = [&](std::chrono::microseconds rtt) { measured_rtt = rtt; };

    Session session(transport, std::move(cbs));

    feed_text(session, make_ctrl_frame(
        {{"Event", "culpeo.init"}, {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}}, basic_init_body()));

    // Server sends ping
    auto ping_result = session.send_ping();
    REQUIRE(ping_result.has_value());
    REQUIRE(transport.any_text_contains("culpeo.ping"));

    // Extract the nonce from the sent ping body (SEC-016: ping uses random nonce)
    const auto ping_text = transport.text_frame_str(transport.text_count() - 1);
    auto nonce_pos = ping_text.find(R"("nonce":)");
    REQUIRE(nonce_pos != std::string::npos);
    const std::string nonce_str = ping_text.substr(nonce_pos + 8, 20);  // Up to 20 digits
    const uint64_t nonce_val = std::stoull(nonce_str);

    // Client responds with pong echoing the nonce
    std::string pong_body = R"({"nonce":)" + std::to_string(nonce_val) + '}';
    auto pong_raw = make_ctrl_frame(
        {{"Event", "culpeo.pong"}, {"Content-Type", "application/json"}},
        pong_body);

    auto pong_result = feed_text(session, pong_raw);
    REQUIRE(pong_result.has_value());

    // RTT callback should have been invoked
    REQUIRE(measured_rtt.has_value());
    // RTT should be ≥ 0 (may be slightly negative in theory due to clock resolution but
    // practically always non-negative)
    CHECK(measured_rtt->count() >= 0);
}

// ─── Auth refresh ─────────────────────────────────────────────────────────────

TEST_CASE("send_auth_refresh: issues challenge, response with correct nonce accepted", "[session]") {
    MockTransport transport;
    SessionCallbacks cbs;
    cbs.on_auth_validate = [](std::string_view) { return true; };
    bool refresh_accepted = false;
    cbs.on_auth_response = [&](std::string_view) {
        refresh_accepted = true;
        return true;
    };

    Session session(transport, std::move(cbs));

    feed_text(session, make_ctrl_frame(
        {{"Event", "culpeo.init"}, {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}}, basic_init_body()));

    // Issue auth-refresh
    auto ref_result = session.send_auth_refresh();
    REQUIRE(ref_result.has_value());
    REQUIRE(transport.any_text_contains("culpeo.auth-refresh"));
    REQUIRE(transport.any_text_contains("nonce"));

    // Extract nonce from the auth-refresh frame
    const auto& auth_refresh_frame = transport.text_frame_str(transport.text_count() - 1);
    auto nonce_pos = auth_refresh_frame.find(R"("nonce":")");
    REQUIRE(nonce_pos != std::string::npos);
    const auto nonce_start = nonce_pos + 9;
    const auto nonce_end = auth_refresh_frame.find('"', nonce_start);
    const std::string nonce_val = auth_refresh_frame.substr(nonce_start, nonce_end - nonce_start);

    // Client responds with echoed nonce
    std::string response_body = R"({"nonce":")" + nonce_val + R"("})";
    auto response_raw = make_ctrl_frame(
        {{"Event", "culpeo.auth-response"},
         {"Authorization", "Bearer new-token"},
         {"Content-Type", "application/json"}},
        response_body);

    auto auth_resp = feed_text(session, response_raw);
    REQUIRE(auth_resp.has_value());
    CHECK(refresh_accepted);
    CHECK(session.state() == SessionState::established);
}

TEST_CASE("auth-refresh: wrong nonce → session closed with auth-expired", "[session]") {
    MockTransport transport;
    SessionCallbacks cbs;
    cbs.on_auth_validate = [](std::string_view) { return true; };
    Session session(transport, std::move(cbs));

    feed_text(session, make_ctrl_frame(
        {{"Event", "culpeo.init"}, {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}}, basic_init_body()));

    // Issue auth-refresh
    session.send_auth_refresh();

    // Respond with wrong nonce (64 hex chars of zeros)
    const std::string wrong_nonce(64, '0');
    auto response_raw = make_ctrl_frame(
        {{"Event", "culpeo.auth-response"},
         {"Authorization", "Bearer new-token"},
         {"Content-Type", "application/json"}},
        R"({"nonce":")" + wrong_nonce + R"("})");

    auto result = feed_text(session, response_raw);
    REQUIRE_FALSE(result.has_value());
    CHECK((result.error() == Error::nonce_mismatch || result.error() == Error::nonce_expired));
    CHECK(session.state() == SessionState::closed);
    CHECK(transport.any_text_contains("auth-expired"));
}

TEST_CASE("auth-refresh: double challenge rejected", "[session]") {
    MockTransport transport;
    SessionCallbacks cbs;
    cbs.on_auth_validate = [](std::string_view) { return true; };

    SessionConfig cfg;
    cfg.min_auth_refresh_interval_s = 0;  // No cooldown for test
    Session session(transport, std::move(cbs), cfg);

    feed_text(session, make_ctrl_frame(
        {{"Event", "culpeo.init"}, {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}}, basic_init_body()));

    // First challenge succeeds
    auto r1 = session.send_auth_refresh();
    REQUIRE(r1.has_value());

    // Second challenge while first is pending → error
    auto r2 = session.send_auth_refresh();
    REQUIRE_FALSE(r2.has_value());
    CHECK(r2.error() == Error::nonce_already_pending);
}

TEST_CASE("auth-response without outstanding challenge → protocol error", "[session]") {
    MockTransport transport;
    SessionCallbacks cbs;
    cbs.on_auth_validate = [](std::string_view) { return true; };
    Session session(transport, std::move(cbs));

    feed_text(session, make_ctrl_frame(
        {{"Event", "culpeo.init"}, {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}}, basic_init_body()));

    // Send auth-response with no outstanding challenge
    auto response_raw = make_ctrl_frame(
        {{"Event", "culpeo.auth-response"},
         {"Authorization", "Bearer new-token"},
         {"Content-Type", "application/json"}},
        R"({"nonce":"abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"})");

    auto result = feed_text(session, response_raw);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::protocol_error);
    CHECK(session.state() == SessionState::closed);
}

// ─── Close ────────────────────────────────────────────────────────────────────

TEST_CASE("culpeo.close received → session sends close back and closes transport", "[session]") {
    MockTransport transport;
    SessionCallbacks cbs;
    cbs.on_auth_validate = [](std::string_view) { return true; };
    std::string close_code_received;
    cbs.on_close = [&](std::string_view code, std::string_view) {
        close_code_received = code;
    };

    Session session(transport, std::move(cbs));

    feed_text(session, make_ctrl_frame(
        {{"Event", "culpeo.init"}, {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}}, basic_init_body()));

    auto close_raw = make_ctrl_frame(
        {{"Event", "culpeo.close"},
         {"Code", "normal"},
         {"Reason", "Done"},
         {"Content-Type", "application/json"}},
        "{}");

    auto result = feed_text(session, close_raw);
    REQUIRE(result.has_value());

    CHECK(session.state() == SessionState::closed);
    CHECK(transport.any_text_contains("culpeo.close"));
    CHECK(transport.closed);
    CHECK(close_code_received == "normal");
}

TEST_CASE("Session::close sends culpeo.close and closes transport", "[session]") {
    MockTransport transport;
    SessionCallbacks cbs;
    cbs.on_auth_validate = [](std::string_view) { return true; };
    Session session(transport, std::move(cbs));

    feed_text(session, make_ctrl_frame(
        {{"Event", "culpeo.init"}, {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}}, basic_init_body()));

    session.close("normal", "Done");

    CHECK(session.state() == SessionState::closed);
    CHECK(transport.any_text_contains("culpeo.close"));
    CHECK(transport.closed);
}

// ─── Frame ordering invariants ────────────────────────────────────────────────

TEST_CASE("Non-init frame before culpeo.init → protocol error", "[session]") {
    MockTransport transport;
    Session session(transport);

    auto ping_raw = make_ctrl_frame(
        {{"Event", "culpeo.ping"}, {"Content-Type", "application/json"}},
        R"({"ts":1000})");

    auto result = feed_text(session, ping_raw);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::protocol_error);
    CHECK(session.state() == SessionState::closed);
}

TEST_CASE("culpeo.init received after established → protocol error", "[session]") {
    MockTransport transport;
    SessionCallbacks cbs;
    cbs.on_auth_validate = [](std::string_view) { return true; };
    Session session(transport, std::move(cbs));

    // First init
    feed_text(session, make_ctrl_frame(
        {{"Event", "culpeo.init"}, {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}}, basic_init_body()));

    REQUIRE(session.state() == SessionState::established);

    // Second init — should fail
    auto result = feed_text(session, make_ctrl_frame(
        {{"Event", "culpeo.init"}, {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}}, basic_init_body()));

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::protocol_error);
    CHECK(session.state() == SessionState::closed);
}

TEST_CASE("Media frame on established session with unknown stream → stream_not_found", "[session]") {
    MockTransport transport;
    SessionCallbacks cbs;
    cbs.on_auth_validate = [](std::string_view) { return true; };
    Session session(transport, std::move(cbs));

    feed_text(session, make_ctrl_frame(
        {{"Event", "culpeo.init"}, {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}}, basic_init_body()));

    auto raw = make_media_frame(
        {{"Stream-Id", "nonexistent"},
         {"Offset", "0"},
         {"Content-Type", "audio/opus"},
         {"Timestamp", "0"}},
        std::string(10, '\0'));

    auto result = feed_binary(session, raw);
    REQUIRE_FALSE(result.has_value());
    // stream_not_found or invalid_direction (both map to protocol error)
    CHECK((result.error() == Error::stream_not_found || result.error() == Error::invalid_direction));
}

// ─── Session resumption ───────────────────────────────────────────────────────

TEST_CASE("Resumption: client reconnects with valid session-id and offsets", "[session]") {
    // Step 1: create original session and advance offsets
    MockTransport transport1;
    SessionCallbacks cbs1;
    cbs1.on_auth_validate = [](std::string_view) { return true; };
    cbs1.on_media_received = [](const StreamInfo&, uint64_t, std::span<const std::byte>) {};
    Session session1(transport1, std::move(cbs1));

    feed_text(session1, make_ctrl_frame(
        {{"Event", "culpeo.init"}, {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}},
        R"({"version":"0.3","streams":[
            {"content_type":"audio/pcm;rate=16000;channels=1;bits=16","type":"input","purpose":"voice","offset_type":"time"}
        ]})"));

    REQUIRE(session1.state() == SessionState::established);
    const std::string session_id = *session1.session_id();
    auto streams1 = session1.streams();
    const std::string sid = streams1[0].id;

    // Feed some PCM frames to advance offset
    for (int i = 0; i < 3; ++i) {
        const std::string payload(320, '\0');  // 160 samples per frame
        const std::string off_str = std::to_string(static_cast<uint64_t>(i * 160));
        auto raw = make_media_frame(
            {{"Stream-Id", sid},
             {"Offset", off_str},
             {"Content-Type", "audio/pcm;rate=16000;channels=1;bits=16"},
             {"Timestamp", "0"}},
            payload);
        feed_binary(session1, raw);
    }

    auto saved = session1.export_state();
    REQUIRE(saved.has_value());
    CHECK(saved->session_id == session_id);
    CHECK(saved->streams[0].offset == 480);  // 3 * 160

    // Step 2: new connection with resumption
    MockTransport transport2;
    SessionCallbacks cbs2;
    cbs2.on_auth_validate = [](std::string_view) { return true; };
    Session session2(transport2, std::move(cbs2), {}, std::move(saved));

    std::string resume_body = R"({"version":"0.3","streams":[
        {"content_type":"audio/pcm;rate=16000;channels=1;bits=16","type":"input","purpose":"voice","offset_type":"time",
         "id":")" + sid + R"(","resume_offset":320}
    ]})";

    auto resume_raw = make_ctrl_frame(
        {{"Event", "culpeo.init"},
         {"Authorization", "Bearer token"},
         {"Session-Id", session_id},
         {"Content-Type", "application/json"}},
        resume_body);

    auto result = feed_text(session2, resume_raw);
    REQUIRE(result.has_value());

    CHECK(session2.state() == SessionState::established);
    // The stream should have been restored with the requested resume_offset
    auto streams2 = session2.streams();
    REQUIRE(streams2.size() == 1);
    CHECK(streams2[0].offset == 320);  // Requested resume_offset

    // init-ack should include resume_offset
    REQUIRE(transport2.any_text_contains("culpeo.init-ack"));
}

TEST_CASE("Resumption: resume_offset > server offset → invalid-streams", "[session]") {
    MockTransport transport1;
    SessionCallbacks cbs1;
    cbs1.on_auth_validate = [](std::string_view) { return true; };
    Session session1(transport1, std::move(cbs1));

    feed_text(session1, make_ctrl_frame(
        {{"Event", "culpeo.init"}, {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}},
        R"({"version":"0.3","streams":[{"content_type":"audio/opus","type":"input","purpose":"audio","offset_type":"message"}]})"));

    const std::string session_id = *session1.session_id();
    auto saved = session1.export_state();
    REQUIRE(saved.has_value());
    // offset is 0 for Opus stream (no frames sent)

    MockTransport transport2;
    SessionCallbacks cbs2;
    cbs2.on_auth_validate = [](std::string_view) { return true; };
    Session session2(transport2, std::move(cbs2), {}, std::move(saved));

    // Request resume_offset=999 but server only has 0
    std::string resume_body = R"({"version":"0.3","streams":[
        {"content_type":"audio/opus","type":"input","purpose":"audio","offset_type":"message","resume_offset":999}
    ]})";

    auto result = feed_text(session2, make_ctrl_frame(
        {{"Event", "culpeo.init"}, {"Authorization", "Bearer token"},
         {"Session-Id", session_id}, {"Content-Type", "application/json"}},
        resume_body));

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::invalid_streams);
    CHECK(session2.state() == SessionState::closed);
    CHECK(transport2.any_text_contains("culpeo.init-error"));
}

TEST_CASE("Resumption: unknown session-id → invalid-session", "[session]") {
    MockTransport transport;
    SessionCallbacks cbs;
    cbs.on_auth_validate = [](std::string_view) { return true; };
    // No prior_state provided
    Session session(transport, std::move(cbs));

    auto result = feed_text(session, make_ctrl_frame(
        {{"Event", "culpeo.init"}, {"Authorization", "Bearer token"},
         {"Session-Id", "deadbeefdeadbeefdeadbeefdeadbeef"},
         {"Content-Type", "application/json"}},
        basic_init_body()));

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::session_expired);
    CHECK(transport.any_text_contains("invalid-session"));
}

// ─── offset_type enforcement ──────────────────────────────────────────────────

TEST_CASE("Missing offset_type → invalid-streams error", "[session][offset_type]") {
    MockTransport transport;
    Session session(transport);

    auto raw = make_ctrl_frame(
        {{"Event", "culpeo.init"},
         {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}},
        // PCM stream without offset_type field
        R"({"version":"0.3","streams":[{"content_type":"audio/pcm;rate=16000;channels=1;bits=16","type":"input"}]})");

    auto result = feed_text(session, raw);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::invalid_streams);
    CHECK(transport.any_text_contains("culpeo.init-error"));
    CHECK(transport.any_text_contains("invalid-streams"));
    CHECK(session.state() == SessionState::closed);
}

TEST_CASE("Unknown offset_type value → invalid-streams error", "[session][offset_type]") {
    MockTransport transport;
    Session session(transport);

    auto raw = make_ctrl_frame(
        {{"Event", "culpeo.init"},
         {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}},
        R"({"version":"0.3","streams":[{"content_type":"audio/opus","type":"input","offset_type":"samples"}]})");

    auto result = feed_text(session, raw);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::invalid_streams);
    CHECK(transport.any_text_contains("culpeo.init-error"));
    CHECK(session.state() == SessionState::closed);
}

TEST_CASE("offset_type=byte: offset increments by payload byte length", "[session][offset_type]") {
    MockTransport transport;
    SessionCallbacks cbs;
    cbs.on_auth_validate = [](std::string_view) { return true; };
    cbs.on_media_received = [](const StreamInfo&, uint64_t, std::span<const std::byte>) {};
    Session session(transport, std::move(cbs));

    feed_text(session, make_ctrl_frame(
        {{"Event", "culpeo.init"}, {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}},
        R"({"version":"0.3","streams":[{"content_type":"audio/opus","type":"input","offset_type":"byte"}]})"));

    REQUIRE(session.state() == SessionState::established);
    auto streams = session.streams();
    const std::string sid = streams[0].id;

    // First frame: 100 bytes → offset should advance to 100
    auto raw1 = make_media_frame(
        {{"Stream-Id", sid}, {"Offset", "0"},
         {"Content-Type", "audio/opus"}, {"Timestamp", "0"}},
        std::string(100, '\x42'));
    auto r1 = feed_binary(session, raw1);
    REQUIRE(r1.has_value());
    CHECK(session.streams()[0].offset == 100);

    // Second frame: 200 bytes → offset should advance to 300
    auto raw2 = make_media_frame(
        {{"Stream-Id", sid}, {"Offset", "100"},
         {"Content-Type", "audio/opus"}, {"Timestamp", "100"}},
        std::string(200, '\xAB'));
    auto r2 = feed_binary(session, raw2);
    REQUIRE(r2.has_value());
    CHECK(session.streams()[0].offset == 300);
}

TEST_CASE("offset_type=message: offset increments by 1 per frame", "[session][offset_type]") {
    MockTransport transport;
    SessionCallbacks cbs;
    cbs.on_auth_validate = [](std::string_view) { return true; };
    cbs.on_media_received = [](const StreamInfo&, uint64_t, std::span<const std::byte>) {};
    Session session(transport, std::move(cbs));

    feed_text(session, make_ctrl_frame(
        {{"Event", "culpeo.init"}, {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}},
        R"({"version":"0.3","streams":[{"content_type":"audio/opus","type":"input","offset_type":"message"}]})"));

    REQUIRE(session.state() == SessionState::established);
    auto streams = session.streams();
    const std::string sid = streams[0].id;

    // Three frames of varying sizes; each should increment offset by 1
    for (int i = 0; i < 3; ++i) {
        auto raw = make_media_frame(
            {{"Stream-Id", sid}, {"Offset", std::to_string(i)},
             {"Content-Type", "audio/opus"}, {"Timestamp", "0"}},
            std::string(50 + i * 10, '\x00'));
        auto r = feed_binary(session, raw);
        REQUIRE(r.has_value());
    }
    CHECK(session.streams()[0].offset == 3);
}

TEST_CASE("offset_type=time: offset increments by sample count (PCM formula)", "[session][offset_type]") {
    MockTransport transport;
    SessionCallbacks cbs;
    cbs.on_auth_validate = [](std::string_view) { return true; };
    cbs.on_media_received = [](const StreamInfo&, uint64_t, std::span<const std::byte>) {};
    Session session(transport, std::move(cbs));

    // 16-bit mono at 16 kHz: 2 bytes per sample
    feed_text(session, make_ctrl_frame(
        {{"Event", "culpeo.init"}, {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}},
        R"({"version":"0.3","streams":[{"content_type":"audio/pcm;rate=16000;channels=1;bits=16","type":"input","offset_type":"time"}]})"));

    REQUIRE(session.state() == SessionState::established);
    auto streams = session.streams();
    const std::string sid = streams[0].id;

    // 320 bytes / 2 bytes_per_sample = 160 samples
    auto raw = make_media_frame(
        {{"Stream-Id", sid}, {"Offset", "0"},
         {"Content-Type", "audio/pcm;rate=16000;channels=1;bits=16"},
         {"Timestamp", "0"}},
        std::string(320, '\x00'));
    auto r = feed_binary(session, raw);
    REQUIRE(r.has_value());
    CHECK(session.streams()[0].offset == 160);
}

TEST_CASE("init-ack includes offset_type in stream list", "[session][offset_type]") {
    MockTransport transport;
    SessionCallbacks cbs;
    cbs.on_auth_validate = [](std::string_view) { return true; };
    Session session(transport, std::move(cbs));

    feed_text(session, make_ctrl_frame(
        {{"Event", "culpeo.init"}, {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}},
        R"({"version":"0.3","streams":[{"content_type":"audio/pcm;rate=16000;channels=1;bits=16","type":"input","offset_type":"time"}]})"));

    REQUIRE(session.state() == SessionState::established);
    REQUIRE(transport.text_count() >= 1);

    // The init-ack body should contain "offset_type":"time"
    const auto init_ack = transport.text_frame_str(0);
    CHECK(init_ack.find("offset_type") != std::string::npos);
    CHECK(init_ack.find("time") != std::string::npos);
}

// ─── Event name validation (§9.5) ────────────────────────────────────────────

TEST_CASE("Invalid event name syntax → invalid_event_name", "[session]") {
    MockTransport transport;
    Session session(transport);

    // "culpeo..init" has consecutive dots → invalid
    std::string raw_frame = "Event: culpeo..init\r\nContent-Type: application/json\r\n\r\n{}";
    auto parsed = frame::parse_headers(frame::FrameType::control, raw_frame);
    REQUIRE(parsed.has_value());

    auto result = session.process_control_frame(*parsed);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::invalid_event_name);
}

// ─── State inspection and export ─────────────────────────────────────────────

TEST_CASE("export_state returns nullopt when no session established", "[session]") {
    MockTransport transport;
    Session session(transport);
    CHECK_FALSE(session.export_state().has_value());
}

TEST_CASE("export_state returns valid state after establishment", "[session]") {
    MockTransport transport;
    SessionCallbacks cbs;
    cbs.on_auth_validate = [](std::string_view) { return true; };
    Session session(transport, std::move(cbs));

    feed_text(session, make_ctrl_frame(
        {{"Event", "culpeo.init"}, {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}}, basic_init_body()));

    auto state = session.export_state();
    REQUIRE(state.has_value());
    CHECK_FALSE(state->session_id.empty());
    CHECK(state->streams.size() == 1);
    CHECK(state->buffer_window_ms > 0);
}

// ─── Thread safety ────────────────────────────────────────────────────────────

TEST_CASE("Concurrent send_media from multiple threads is safe", "[session][thread-safety]") {
    MockTransport transport;
    SessionCallbacks cbs;
    cbs.on_auth_validate = [](std::string_view) { return true; };
    Session session(transport, std::move(cbs));

    // Establish with duplex stream
    feed_text(session, make_ctrl_frame(
        {{"Event", "culpeo.init"}, {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}},
        R"({"version":"0.3","streams":[{"content_type":"audio/opus","type":"duplex","offset_type":"message"}]})"));

    REQUIRE(session.state() == SessionState::established);
    const std::string sid = session.streams()[0].id;

    constexpr int kThreads = 4;
    constexpr int kSendsPerThread = 25;
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            const std::string payload(120, '\x42');
            for (int i = 0; i < kSendsPerThread; ++i) {
                auto result = session.send_media(sid,
                    culpeo::message::as_bytes(payload), 0);
                if (result.has_value()) {
                    ++success_count;
                }
            }
        });
    }

    for (auto& t : threads) t.join();

    // All sends should have succeeded (no data races, no crashes)
    const int expected = kThreads * kSendsPerThread;
    CHECK(success_count.load() == expected);
    CHECK(static_cast<int>(transport.binary_count()) == expected);
}

TEST_CASE("Concurrent process and send from different threads is safe", "[session][thread-safety]") {
    MockTransport transport;
    SessionCallbacks cbs;
    cbs.on_auth_validate = [](std::string_view) { return true; };
    cbs.on_media_received = [](const StreamInfo&, uint64_t, std::span<const std::byte>) {};
    Session session(transport, std::move(cbs));

    feed_text(session, make_ctrl_frame(
        {{"Event", "culpeo.init"}, {"Authorization", "Bearer token"},
         {"Content-Type", "application/json"}},
        R"({"version":"0.3","streams":[
            {"content_type":"audio/pcm;rate=16000;channels=1;bits=16","type":"input","purpose":"in","offset_type":"time"},
            {"content_type":"audio/opus","type":"output","purpose":"out","offset_type":"message"}
        ]})"));

    REQUIRE(session.state() == SessionState::established);

    std::string input_sid, output_sid;
    for (const auto& s : session.streams()) {
        if (s.direction == StreamDirection::input) input_sid = s.id;
        else output_sid = s.id;
    }

    constexpr int kFrames = 20;
    std::atomic<bool> done{false};

    // Thread 1: receive media from client
    std::thread receiver([&] {
        for (int i = 0; i < kFrames && !done; ++i) {
            const std::string payload(160, '\0');  // 80 samples, 16-bit mono
            const std::string off_str = std::to_string(static_cast<uint64_t>(i * 80));
            auto raw = make_media_frame(
                {{"Stream-Id", input_sid},
                 {"Offset", off_str},
                 {"Content-Type", "audio/pcm;rate=16000;channels=1;bits=16"},
                 {"Timestamp", "0"}},
                payload);
            auto parsed = frame::parse_headers(frame::FrameType::media, raw);
            if (parsed) session.process_media_frame(*parsed);
        }
    });

    // Thread 2: send media to client
    std::thread sender([&] {
        const std::string payload(120, '\x42');
        for (int i = 0; i < kFrames; ++i) {
            session.send_media(output_sid, culpeo::message::as_bytes(payload), 0);
        }
        done = true;
    });

    receiver.join();
    sender.join();

    // No crashes, no assertion failures = thread safety holds
    CHECK(session.state() == SessionState::established);
}
