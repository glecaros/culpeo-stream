#include <catch2/catch_test_macros.hpp>

#include "culpeo/message.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace culpeo::message;

namespace {

[[nodiscard]] std::string bytes_to_string(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

}  // namespace

TEST_CASE("Parse control headers zero-copy", "[parser]") {
    const std::string frame =
        "Event: culpeo.init\r\n"
        "Content-Type: application/json\r\n"
        "Buffer-Window: 2500\r\n"
        "X-Ignored: value\r\n"
        "\r\n"
        "{}";

    auto parsed = parse_headers(FrameType::control, std::string_view(frame));
    REQUIRE(parsed.has_value());
    CHECK(parsed->frame_type == FrameType::control);
    CHECK(parsed->event == std::optional<std::string_view>{"culpeo.init"});
    CHECK(parsed->content_type == std::optional<std::string_view>{"application/json"});
    CHECK(parsed->buffer_window == std::optional<std::string_view>{"2500"});
    CHECK(parsed->body == "{}");

    SECTION("views reference original buffer") {
        REQUIRE(parsed->event.has_value());
        CHECK(parsed->event->data() >= frame.data());
        CHECK(parsed->event->data() < frame.data() + frame.size());
    }
}

TEST_CASE("Parse media headers with binary body", "[parser]") {
    std::string frame =
        "Stream-Id: s1\r\n"
        "Offset: 99\r\n"
        "Content-Type: audio/opus\r\n"
        "Timestamp: 12345\r\n"
        "\r\n";
    frame.push_back(static_cast<char>(0x00));
    frame.push_back(static_cast<char>(0x7f));
    frame.push_back(static_cast<char>(0xff));

    auto parsed = parse_headers(FrameType::media, std::string_view(frame.data(), frame.size()));
    REQUIRE(parsed.has_value());
    CHECK(parsed->stream_id == std::optional<std::string_view>{"s1"});
    CHECK(parsed->offset == std::optional<std::string_view>{"99"});
    CHECK(parsed->body.size() == 3U);
    auto body = parsed->body_bytes();
    CHECK(std::to_integer<unsigned int>(body[0]) == 0U);
    CHECK(std::to_integer<unsigned int>(body[1]) == 0x7fU);
    CHECK(std::to_integer<unsigned int>(body[2]) == 0xffU);
}

TEST_CASE("Parse from byte span", "[parser]") {
    constexpr std::string_view frame = "Event: culpeo.ping\r\n\r\n{}";
    auto parsed = parse_headers(FrameType::control, as_bytes(frame));
    REQUIRE(parsed.has_value());
    CHECK(parsed->event == std::optional<std::string_view>{"culpeo.ping"});
}

TEST_CASE("Parse header optional whitespace after colon", "[parser]") {
    SECTION("no space") {
        constexpr std::string_view frame = "Event:culpeo.ping\r\n\r\n{}";
        auto parsed = parse_headers(FrameType::control, frame);
        REQUIRE(parsed.has_value());
        CHECK(parsed->event == std::optional<std::string_view>{"culpeo.ping"});
    }

    SECTION("tab and space") {
        constexpr std::string_view frame = "Event:\t culpeo.ping\r\n\r\n{}";
        auto parsed = parse_headers(FrameType::control, frame);
        REQUIRE(parsed.has_value());
        CHECK(parsed->event == std::optional<std::string_view>{"culpeo.ping"});
    }
}

TEST_CASE("Reject missing terminator", "[parser][error]") {
    constexpr std::string_view frame = "Event: culpeo.close\r\nContent-Type: application/json";
    auto parsed = parse_headers(FrameType::control, frame);
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error() == Error::missing_header_terminator);
}

TEST_CASE("Reject header block too large", "[parser][error]") {
    std::string frame = "Event: ";
    frame.append(9000, 'a');

    auto parsed = parse_headers(FrameType::control, std::string_view(frame), ParseLimits{});
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error() == Error::header_block_too_large);
}

TEST_CASE("Reject oversized header value", "[parser][error]") {
    std::string frame = "Reason: ";
    frame.append(4100, 'x');
    frame.append("\r\n\r\n{}");

    auto parsed = parse_headers(FrameType::control, std::string_view(frame), ParseLimits{});
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error() == Error::invalid_header_value);
}

TEST_CASE("Reject null byte in header name", "[parser][error]") {
    std::string frame{"Even", 4};
    frame.push_back('\0');
    frame.append("t: culpeo.init\r\n\r\n{}");

    auto parsed = parse_headers(FrameType::control, std::string_view(frame.data(), frame.size()));
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error() == Error::invalid_header_name);
}

TEST_CASE("Reject folded header value", "[parser][error]") {
    const std::string frame =
        "Reason: line1\r\n"
        " line2\r\n"
        "\r\n"
        "{}";

    auto parsed = parse_headers(FrameType::control, std::string_view(frame));
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error() == Error::invalid_header_line);
}

TEST_CASE("Reject duplicate reserved header", "[parser][error]") {
    const std::string frame =
        "Event: culpeo.ping\r\n"
        "event: culpeo.pong\r\n"
        "\r\n"
        "{}";

    auto parsed = parse_headers(FrameType::control, std::string_view(frame));
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error() == Error::duplicate_header);
}

TEST_CASE("Serialize to caller buffer", "[serializer]") {
    constexpr HeaderFieldView headers[]{
        {"Event", "culpeo.close"},
        {"Code", "normal"},
    };
    constexpr std::string_view body = "{}";
    std::vector<std::byte> buffer(serialized_frame_size(headers, body.size()));

    auto written = serialize_frame_to_buffer(
        FrameType::control, headers, as_bytes(body), buffer);

    REQUIRE(written.has_value());
    CHECK(*written == buffer.size());
    CHECK(bytes_to_string(buffer) == "Event: culpeo.close\r\nCode: normal\r\n\r\n{}");
}

TEST_CASE("Serialize rejects small buffer", "[serializer][error]") {
    constexpr HeaderFieldView headers[]{{"Event", "culpeo.ping"}};
    std::vector<std::byte> buffer(4);

    auto written = serialize_frame_to_buffer(FrameType::control, headers, {}, buffer);
    REQUIRE_FALSE(written.has_value());
    CHECK(written.error() == Error::buffer_too_small);
}

TEST_CASE("Serialize vector preserves binary body", "[serializer]") {
    constexpr HeaderFieldView headers[]{
        {"Stream-Id", "stream-1"},
        {"Offset", "7"},
        {"Content-Type", "audio/aac"},
    };
    const std::array<std::byte, 4> body{
        std::byte{0x10}, std::byte{0x00}, std::byte{0x22}, std::byte{0xff}};

    auto serialized = serialize_frame(FrameType::media, headers, body);
    REQUIRE(serialized.has_value());
    CHECK(serialized->size() == serialized_frame_size(headers, body.size()));
    CHECK((*serialized)[serialized->size() - 4] == std::byte{0x10});
    CHECK((*serialized)[serialized->size() - 1] == std::byte{0xff});
}

TEST_CASE("Serialize rejects invalid header value", "[serializer][error]") {
    constexpr HeaderFieldView headers[]{{"Reason", "bad\r\nvalue"}};
    auto serialized = serialize_frame(FrameType::control, headers, {});
    REQUIRE_FALSE(serialized.has_value());
    CHECK(serialized.error() == Error::invalid_header_value);
}

TEST_CASE("Parse PCM content type", "[content-type]") {
    auto parsed = parse_content_type("audio/pcm; channels=1; bits=16; rate=16000");
    REQUIRE(parsed.has_value());
    REQUIRE(std::holds_alternative<AudioPcmContentType>(*parsed));
    const auto pcm = std::get<AudioPcmContentType>(*parsed);
    CHECK(pcm.rate == 16000U);
    CHECK(pcm.channels == 1U);
    CHECK(pcm.bits == 16U);
}

TEST_CASE("Parse application/json content type", "[content-type]") {
    auto parsed = parse_content_type("application/json");
    REQUIRE(parsed.has_value());
    CHECK(std::holds_alternative<ApplicationJsonContentType>(*parsed));
}

TEST_CASE("Parse known audio content types", "[content-type]") {
    SECTION("audio/opus") {
        auto parsed = parse_content_type("audio/opus");
        REQUIRE(parsed.has_value());
        CHECK(std::holds_alternative<AudioOpusContentType>(*parsed));
    }

    SECTION("audio/aac") {
        auto parsed = parse_content_type("audio/aac");
        REQUIRE(parsed.has_value());
        CHECK(std::holds_alternative<AudioAacContentType>(*parsed));
    }
}

TEST_CASE("Parse unknown content type", "[content-type]") {
    auto parsed = parse_content_type("video/h264;profile=baseline");
    REQUIRE(parsed.has_value());
    REQUIRE(std::holds_alternative<UnknownContentType>(*parsed));
    const auto unknown = std::get<UnknownContentType>(*parsed);
    CHECK(unknown.type == "video");
    CHECK(unknown.subtype == "h264");
}

TEST_CASE("Reject missing PCM parameter", "[content-type][error]") {
    auto parsed = parse_content_type("audio/pcm;rate=16000;channels=1");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error() == Error::missing_content_type_parameter);
}

TEST_CASE("Reject invalid PCM numeric parameter", "[content-type][error]") {
    auto parsed = parse_content_type("audio/pcm;rate=fast;channels=1;bits=16");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error() == Error::invalid_numeric_parameter);
}

TEST_CASE("Reject duplicate PCM parameter", "[content-type][error]") {
    auto parsed = parse_content_type("audio/pcm;rate=16000;rate=8000;channels=1;bits=16");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error() == Error::invalid_content_type);
}
