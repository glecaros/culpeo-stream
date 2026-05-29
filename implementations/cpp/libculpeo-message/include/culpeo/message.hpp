#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace culpeo::message {

enum class FrameType {
    control,
    media,
};

enum class Error {
    missing_header_terminator,
    header_block_too_large,
    invalid_header_line,
    invalid_header_name,
    invalid_header_value,
    duplicate_header,
    invalid_content_type,
    invalid_numeric_parameter,
    missing_content_type_parameter,
    buffer_too_small,
    invalid_frame_type,
};

[[nodiscard]] constexpr std::string_view error_message(Error error) noexcept {
    switch (error) {
    case Error::missing_header_terminator:
        return "missing header terminator";
    case Error::header_block_too_large:
        return "header block exceeds configured limit";
    case Error::invalid_header_line:
        return "invalid header line";
    case Error::invalid_header_name:
        return "invalid header name";
    case Error::invalid_header_value:
        return "invalid header value";
    case Error::duplicate_header:
        return "duplicate reserved header";
    case Error::invalid_content_type:
        return "invalid content type";
    case Error::invalid_numeric_parameter:
        return "invalid numeric parameter";
    case Error::missing_content_type_parameter:
        return "missing required content type parameter";
    case Error::buffer_too_small:
        return "output buffer too small";
    case Error::invalid_frame_type:
        return "invalid frame type";
    }

    return "unknown error";
}

struct ParseLimits {
    std::size_t max_header_block_bytes{8192};
    std::size_t max_header_value_bytes{4096};
    std::size_t max_header_count{64};
};

struct HeaderFieldView {
    std::string_view name;
    std::string_view value;
};

[[nodiscard]] inline std::span<const std::byte> as_bytes(std::string_view view) noexcept {
    return {reinterpret_cast<const std::byte*>(view.data()), view.size()};
}

struct ParsedHeadersView {
    // All views reference caller-owned frame storage; the source buffer must outlive this object.
    FrameType frame_type{};
    std::string_view frame;
    std::string_view header_block;
    std::string_view body;

    std::optional<std::string_view> event;
    std::optional<std::string_view> content_type;
    std::optional<std::string_view> authorization;
    std::optional<std::string_view> session_id;
    std::optional<std::string_view> stream_id;
    std::optional<std::string_view> offset;
    std::optional<std::string_view> timestamp;
    std::optional<std::string_view> buffer_window;
    std::optional<std::string_view> reason;
    std::optional<std::string_view> code;

    [[nodiscard]] std::span<const std::byte> body_bytes() const noexcept {
        return as_bytes(body);
    }
};

struct ApplicationJsonContentType {};
struct AudioPcmContentType {
    std::uint32_t rate{};
    std::uint16_t channels{};
    std::uint16_t bits{};
};
struct AudioOpusContentType {};
struct AudioAacContentType {};
struct UnknownContentType {
    std::string_view type;
    std::string_view subtype;
};

using ParsedContentType = std::variant<
    ApplicationJsonContentType,
    AudioPcmContentType,
    AudioOpusContentType,
    AudioAacContentType,
    UnknownContentType>;

// Parsing is zero-copy: returned string_views point into `frame`.
[[nodiscard]] std::expected<ParsedHeadersView, Error>
parse_headers(FrameType frame_type, std::string_view frame, ParseLimits limits = {}) noexcept;

[[nodiscard]] std::expected<ParsedHeadersView, Error>
parse_headers(FrameType frame_type, std::span<const std::byte> frame, ParseLimits limits = {}) noexcept;

[[nodiscard]] std::expected<ParsedContentType, Error>
parse_content_type(std::string_view value) noexcept;

[[nodiscard]] std::size_t serialized_frame_size(
    std::span<const HeaderFieldView> headers,
    std::size_t body_size) noexcept;

[[nodiscard]] std::expected<std::size_t, Error> serialize_frame_to_buffer(
    FrameType frame_type,
    std::span<const HeaderFieldView> headers,
    std::span<const std::byte> body,
    std::span<std::byte> output) noexcept;

[[nodiscard]] std::expected<std::vector<std::byte>, Error> serialize_frame(
    FrameType frame_type,
    std::span<const HeaderFieldView> headers,
    std::span<const std::byte> body) noexcept;

}  // namespace culpeo::message
