#include "culpeo/message.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace culpeo::message {
namespace {

constexpr std::array<std::string_view, 10> kReservedHeaderNames{
    "Event",
    "Content-Type",
    "Authorization",
    "Session-Id",
    "Stream-Id",
    "Offset",
    "Timestamp",
    "Buffer-Window",
    "Reason",
    "Code",
};

[[nodiscard]] constexpr char to_lower_ascii(char value) noexcept {
    return (value >= 'A' && value <= 'Z') ? static_cast<char>(value - 'A' + 'a') : value;
}

[[nodiscard]] constexpr bool iequals_ascii(std::string_view lhs, std::string_view rhs) noexcept {
    return std::ranges::equal(lhs, rhs, [](char a, char b) {
        return to_lower_ascii(a) == to_lower_ascii(b);
    });
}

[[nodiscard]] constexpr bool is_token_char(char value) noexcept {
    return (value >= '0' && value <= '9')
        || (value >= 'A' && value <= 'Z')
        || (value >= 'a' && value <= 'z')
        || value == '!'
        || value == '#'
        || value == '$'
        || value == '%'
        || value == '&'
        || value == '\''
        || value == '*'
        || value == '+'
        || value == '-'
        || value == '.'
        || value == '^'
        || value == '_'
        || value == '`'
        || value == '|'
        || value == '~';
}

[[nodiscard]] constexpr std::string_view trim_ascii(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }

    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }

    return value;
}

[[nodiscard]] constexpr bool valid_header_name(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }

    for (const char ch : value) {
        if (ch == '\0' || !is_token_char(ch)) {
            return false;
        }
    }

    return true;
}

[[nodiscard]] constexpr bool valid_header_value(std::string_view value) noexcept {
    for (const char ch : value) {
        if (ch == '\r' || ch == '\n' || ch == '\0') {
            return false;
        }
    }

    return true;
}

[[nodiscard]] std::expected<std::uint32_t, Error> parse_u32(std::string_view value) noexcept {
    if (value.empty()) {
        return std::unexpected(Error::invalid_numeric_parameter);
    }

    std::uint32_t parsed = 0;
    for (const char ch : value) {
        if (ch < '0' || ch > '9') {
            return std::unexpected(Error::invalid_numeric_parameter);
        }

        const auto digit = static_cast<std::uint32_t>(ch - '0');
        if (parsed > ((std::numeric_limits<std::uint32_t>::max() - digit) / 10U)) {
            return std::unexpected(Error::invalid_numeric_parameter);
        }

        parsed = static_cast<std::uint32_t>(parsed * 10U + digit);
    }

    return parsed;
}

[[nodiscard]] std::expected<std::uint16_t, Error> parse_u16(std::string_view value) noexcept {
    const auto parsed = parse_u32(value);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }

    if (*parsed > std::numeric_limits<std::uint16_t>::max()) {
        return std::unexpected(Error::invalid_numeric_parameter);
    }

    return static_cast<std::uint16_t>(*parsed);
}

[[nodiscard]] std::expected<std::monostate, Error>
set_if_reserved(std::string_view name, std::string_view value, ParsedHeadersView& parsed) noexcept {
    const auto assign = [&](auto& slot) -> std::expected<std::monostate, Error> {
        if (slot.has_value()) {
            return std::unexpected(Error::duplicate_header);
        }
        slot = value;
        return std::monostate{};
    };

    if (iequals_ascii(name, kReservedHeaderNames[0])) {
        return assign(parsed.event);
    }
    if (iequals_ascii(name, kReservedHeaderNames[1])) {
        return assign(parsed.content_type);
    }
    if (iequals_ascii(name, kReservedHeaderNames[2])) {
        return assign(parsed.authorization);
    }
    if (iequals_ascii(name, kReservedHeaderNames[3])) {
        return assign(parsed.session_id);
    }
    if (iequals_ascii(name, kReservedHeaderNames[4])) {
        return assign(parsed.stream_id);
    }
    if (iequals_ascii(name, kReservedHeaderNames[5])) {
        return assign(parsed.offset);
    }
    if (iequals_ascii(name, kReservedHeaderNames[6])) {
        return assign(parsed.timestamp);
    }
    if (iequals_ascii(name, kReservedHeaderNames[7])) {
        return assign(parsed.buffer_window);
    }
    if (iequals_ascii(name, kReservedHeaderNames[8])) {
        return assign(parsed.reason);
    }
    if (iequals_ascii(name, kReservedHeaderNames[9])) {
        return assign(parsed.code);
    }

    return std::monostate{};
}

}  // namespace

std::expected<ParsedHeadersView, Error>
parse_headers(FrameType frame_type, std::string_view frame, ParseLimits limits) noexcept {
    switch (frame_type) {
    case FrameType::control:
    case FrameType::media:
        break;
    default:
        return std::unexpected(Error::invalid_frame_type);
    }

    const auto search_length =
        frame.size() < (limits.max_header_block_bytes + 4U) ? frame.size() : (limits.max_header_block_bytes + 4U);
    const auto terminator = frame.substr(0, search_length).find("\r\n\r\n");
    if (terminator == std::string_view::npos) {
        if (frame.size() > limits.max_header_block_bytes) {
            return std::unexpected(Error::header_block_too_large);
        }
        return std::unexpected(Error::missing_header_terminator);
    }

    if (terminator > limits.max_header_block_bytes) {
        return std::unexpected(Error::header_block_too_large);
    }

    ParsedHeadersView parsed{};
    parsed.frame_type = frame_type;
    parsed.frame = frame;
    parsed.header_block = frame.substr(0, terminator);
    parsed.body = frame.substr(terminator + 4U);

    std::size_t line_start = 0;
    std::size_t header_count = 0;
    while (line_start < parsed.header_block.size()) {
        if (header_count >= limits.max_header_count) {
            return std::unexpected(Error::header_block_too_large);
        }
        auto line_end = parsed.header_block.find("\r\n", line_start);
        if (line_end == std::string_view::npos) {
            line_end = parsed.header_block.size();
        }

        const auto line = parsed.header_block.substr(line_start, line_end - line_start);
        if (line.empty()) {
            return std::unexpected(Error::invalid_header_line);
        }

        const auto separator = line.find(':');
        if (separator == std::string_view::npos) {
            return std::unexpected(Error::invalid_header_line);
        }

        const auto name = line.substr(0, separator);
        const auto value = trim_ascii(line.substr(separator + 1U));
        if (!valid_header_name(name)) {
            return std::unexpected(Error::invalid_header_name);
        }
        if (value.size() > limits.max_header_value_bytes || !valid_header_value(value)) {
            return std::unexpected(Error::invalid_header_value);
        }

        const auto reserved_result = set_if_reserved(name, value, parsed);
        if (!reserved_result) {
            return std::unexpected(reserved_result.error());
        }

        ++header_count;
        line_start = line_end + 2U;
    }

    return parsed;
}

std::expected<ParsedHeadersView, Error>
parse_headers(FrameType frame_type, std::span<const std::byte> frame, ParseLimits limits) noexcept {
    return parse_headers(
        frame_type,
        std::string_view(reinterpret_cast<const char*>(frame.data()), frame.size()),
        limits);
}

std::expected<ParsedContentType, Error> parse_content_type(std::string_view value) noexcept {
    value = trim_ascii(value);
    if (value.empty()) {
        return std::unexpected(Error::invalid_content_type);
    }

    const auto first_separator = value.find(';');
    const auto media_type = trim_ascii(value.substr(0, first_separator));
    const auto slash = media_type.find('/');
    if (slash == std::string_view::npos) {
        return std::unexpected(Error::invalid_content_type);
    }

    const auto type = trim_ascii(media_type.substr(0, slash));
    const auto subtype = trim_ascii(media_type.substr(slash + 1U));
    if (!valid_header_name(type) || !valid_header_name(subtype)) {
        return std::unexpected(Error::invalid_content_type);
    }

    if (iequals_ascii(type, "application") && iequals_ascii(subtype, "json")) {
        return ParsedContentType{ApplicationJsonContentType{}};
    }
    if (iequals_ascii(type, "audio") && iequals_ascii(subtype, "opus")) {
        return ParsedContentType{AudioOpusContentType{}};
    }
    if (iequals_ascii(type, "audio") && iequals_ascii(subtype, "aac")) {
        return ParsedContentType{AudioAacContentType{}};
    }
    if (!iequals_ascii(type, "audio") || !iequals_ascii(subtype, "pcm")) {
        return ParsedContentType{UnknownContentType{type, subtype}};
    }

    std::optional<std::uint32_t> rate;
    std::optional<std::uint16_t> channels;
    std::optional<std::uint16_t> bits;

    std::size_t cursor = first_separator == std::string_view::npos ? value.size() : first_separator + 1U;
    while (cursor < value.size()) {
        auto next = value.find(';', cursor);
        if (next == std::string_view::npos) {
            next = value.size();
        }

        auto parameter = trim_ascii(value.substr(cursor, next - cursor));
        if (parameter.empty()) {
            return std::unexpected(Error::invalid_content_type);
        }

        const auto equals = parameter.find('=');
        if (equals == std::string_view::npos) {
            return std::unexpected(Error::invalid_content_type);
        }

        const auto key = trim_ascii(parameter.substr(0, equals));
        const auto numeric_value = trim_ascii(parameter.substr(equals + 1U));
        if (!valid_header_name(key)) {
            return std::unexpected(Error::invalid_content_type);
        }

        if (iequals_ascii(key, "rate")) {
            if (rate.has_value()) {
                return std::unexpected(Error::invalid_content_type);
            }
            auto parsed = parse_u32(numeric_value);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            rate = *parsed;
        } else if (iequals_ascii(key, "channels")) {
            if (channels.has_value()) {
                return std::unexpected(Error::invalid_content_type);
            }
            auto parsed = parse_u16(numeric_value);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            channels = *parsed;
        } else if (iequals_ascii(key, "bits")) {
            if (bits.has_value()) {
                return std::unexpected(Error::invalid_content_type);
            }
            auto parsed = parse_u16(numeric_value);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            bits = *parsed;
        }

        cursor = next + 1U;
    }

    if (!rate.has_value() || !channels.has_value() || !bits.has_value()) {
        return std::unexpected(Error::missing_content_type_parameter);
    }

    return ParsedContentType{AudioPcmContentType{.rate = *rate, .channels = *channels, .bits = *bits}};
}

std::size_t serialized_frame_size(std::span<const HeaderFieldView> headers, std::size_t body_size) noexcept {
    std::size_t size = 2U + body_size;
    for (const auto& header : headers) {
        size += header.name.size() + 2U + header.value.size() + 2U;
    }
    return size;
}

std::expected<std::size_t, Error> serialize_frame_to_buffer(
    FrameType frame_type,
    std::span<const HeaderFieldView> headers,
    std::span<const std::byte> body,
    std::span<std::byte> output) noexcept {
    switch (frame_type) {
    case FrameType::control:
    case FrameType::media:
        break;
    default:
        return std::unexpected(Error::invalid_frame_type);
    }

    const auto required = serialized_frame_size(headers, body.size());
    if (output.size() < required) {
        return std::unexpected(Error::buffer_too_small);
    }

    std::size_t cursor = 0;
    const auto write_bytes = [&](const auto* data, std::size_t size) {
        std::memcpy(output.data() + cursor, data, size);
        cursor += size;
    };

    for (const auto& header : headers) {
        if (!valid_header_name(header.name)) {
            return std::unexpected(Error::invalid_header_name);
        }
        if (!valid_header_value(header.value)) {
            return std::unexpected(Error::invalid_header_value);
        }

        write_bytes(header.name.data(), header.name.size());
        write_bytes(": ", 2U);
        write_bytes(header.value.data(), header.value.size());
        write_bytes("\r\n", 2U);
    }

    write_bytes("\r\n", 2U);
    if (!body.empty()) {
        write_bytes(body.data(), body.size());
    }

    return cursor;
}

std::expected<std::vector<std::byte>, Error> serialize_frame(
    FrameType frame_type,
    std::span<const HeaderFieldView> headers,
    std::span<const std::byte> body) noexcept {
    std::vector<std::byte> output(serialized_frame_size(headers, body.size()));
    auto written = serialize_frame_to_buffer(frame_type, headers, body, output);
    if (!written) {
        return std::unexpected(written.error());
    }

    output.resize(*written);
    return output;
}

}  // namespace culpeo::message
