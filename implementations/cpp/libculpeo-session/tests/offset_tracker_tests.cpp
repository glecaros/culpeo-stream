#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

// Include the internal header directly for unit testing
#include "../src/offset_tracker.hpp"

using namespace culpeo::session;
using namespace culpeo::session::internal;

// ─── compute_pcm_increment ────────────────────────────────────────────────────

TEST_CASE("PCM increment: mono 16-bit", "[offset_tracker]") {
    // 16 bytes at 1 channel, 16-bit = 8 samples
    PcmParams params{.rate = 16000, .channels = 1, .bits = 16};
    auto result = compute_pcm_increment(16, params);
    REQUIRE(result.has_value());
    CHECK(*result == 8u);
}

TEST_CASE("PCM increment: stereo 16-bit", "[offset_tracker]") {
    // 64 bytes / (2 channels * 2 bytes) = 16 samples
    PcmParams params{.rate = 44100, .channels = 2, .bits = 16};
    auto result = compute_pcm_increment(64, params);
    REQUIRE(result.has_value());
    CHECK(*result == 16u);
}

TEST_CASE("PCM increment: stereo 24-bit", "[offset_tracker]") {
    // 48 bytes / (2 * 3 bytes) = 8 samples
    PcmParams params{.rate = 48000, .channels = 2, .bits = 24};
    auto result = compute_pcm_increment(48, params);
    REQUIRE(result.has_value());
    CHECK(*result == 8u);
}

TEST_CASE("PCM increment: frame_bytes not divisible — truncates", "[offset_tracker]") {
    // 17 bytes / (1 * 2) = 8 (integer division)
    PcmParams params{.rate = 16000, .channels = 1, .bits = 16};
    auto result = compute_pcm_increment(17, params);
    REQUIRE(result.has_value());
    CHECK(*result == 8u);
}

TEST_CASE("PCM increment: zero channels → error", "[offset_tracker]") {
    PcmParams params{.rate = 16000, .channels = 0, .bits = 16};
    auto result = compute_pcm_increment(16, params);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::offset_overflow);
}

TEST_CASE("PCM increment: zero bits → error", "[offset_tracker]") {
    PcmParams params{.rate = 16000, .channels = 1, .bits = 0};
    auto result = compute_pcm_increment(16, params);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::offset_overflow);
}

TEST_CASE("PCM increment: bits not multiple of 8 → error", "[offset_tracker]") {
    PcmParams params{.rate = 16000, .channels = 1, .bits = 7};
    auto result = compute_pcm_increment(16, params);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::offset_overflow);
}

TEST_CASE("PCM increment: zero frame_bytes → 0 increment", "[offset_tracker]") {
    PcmParams params{.rate = 16000, .channels = 1, .bits = 16};
    auto result = compute_pcm_increment(0, params);
    REQUIRE(result.has_value());
    CHECK(*result == 0u);
}

// ─── advance_offset ───────────────────────────────────────────────────────────

TEST_CASE("advance_offset: PCM stream advances by sample count", "[offset_tracker]") {
    StreamInfo stream{};
    stream.codec = StreamCodec::pcm;
    stream.pcm_params = PcmParams{.rate = 16000, .channels = 1, .bits = 16};
    stream.offset = 0;

    auto result = advance_offset(stream, 320);  // 160 samples
    REQUIRE(result.has_value());
    CHECK(stream.offset == 160u);
}

TEST_CASE("advance_offset: PCM stream cumulative", "[offset_tracker]") {
    StreamInfo stream{};
    stream.codec = StreamCodec::pcm;
    stream.pcm_params = PcmParams{.rate = 16000, .channels = 1, .bits = 16};
    stream.offset = 0;

    advance_offset(stream, 320);  // +160
    advance_offset(stream, 640);  // +320
    advance_offset(stream, 160);  // +80
    CHECK(stream.offset == 560u);
}

TEST_CASE("advance_offset: Opus stream increments by 1", "[offset_tracker]") {
    StreamInfo stream{};
    stream.codec = StreamCodec::opus;
    stream.offset = 42;

    auto result = advance_offset(stream, 1234);
    REQUIRE(result.has_value());
    CHECK(stream.offset == 43u);
}

TEST_CASE("advance_offset: AAC stream increments by 1", "[offset_tracker]") {
    StreamInfo stream{};
    stream.codec = StreamCodec::aac;
    stream.offset = 100;

    auto result = advance_offset(stream, 512);
    REQUIRE(result.has_value());
    CHECK(stream.offset == 101u);
}

TEST_CASE("advance_offset: unknown codec increments by 1", "[offset_tracker]") {
    StreamInfo stream{};
    stream.codec = StreamCodec::other;
    stream.offset = 7;

    auto result = advance_offset(stream, 64);
    REQUIRE(result.has_value());
    CHECK(stream.offset == 8u);
}

TEST_CASE("advance_offset: PCM missing pcm_params → error", "[offset_tracker]") {
    StreamInfo stream{};
    stream.codec = StreamCodec::pcm;
    stream.pcm_params = std::nullopt;
    stream.offset = 0;

    auto result = advance_offset(stream, 320);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::offset_overflow);
}

TEST_CASE("advance_offset: overflow protection on offset addition", "[offset_tracker]") {
    StreamInfo stream{};
    stream.codec = StreamCodec::opus;
    stream.offset = std::numeric_limits<uint64_t>::max();

    auto result = advance_offset(stream, 1);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::offset_overflow);
}

// ─── check_offset ─────────────────────────────────────────────────────────────

TEST_CASE("check_offset: matches expected → ok", "[offset_tracker]") {
    StreamInfo stream{};
    stream.offset = 1024;

    auto result = check_offset(stream, 1024);
    CHECK(result.has_value());
}

TEST_CASE("check_offset: less than expected → error", "[offset_tracker]") {
    StreamInfo stream{};
    stream.offset = 1024;

    auto result = check_offset(stream, 1023);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::offset_mismatch);
}

TEST_CASE("check_offset: greater than expected → error", "[offset_tracker]") {
    StreamInfo stream{};
    stream.offset = 1024;

    auto result = check_offset(stream, 1025);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::offset_mismatch);
}

TEST_CASE("check_offset: zero offset on fresh stream", "[offset_tracker]") {
    StreamInfo stream{};
    stream.offset = 0;

    auto result = check_offset(stream, 0);
    CHECK(result.has_value());
}
