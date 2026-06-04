/**
 * c_api.cpp — extern "C" shim exposing libculpeo-message to WebAssembly
 * (and any other C consumer compiled with Emscripten or a plain C compiler).
 *
 * This file wraps the C++20 culpeo::message API so that Emscripten can
 * produce a WASM module without the TypeScript side needing to duplicate
 * parsing logic.  All real parsing and validation is performed by
 * culpeo::message::parse_headers / serialize_frame_to_buffer — this shim
 * only adapts the calling convention.
 *
 * Key differences from the old standalone culpeo_parser.c:
 *  - Keys are NOT lowercased in-place; key_ptr/key_len reference the
 *    original casing from the input buffer.  Callers must normalise case
 *    on the JavaScript side (e.g., key.toLowerCase()).
 *  - All validation (header limits, token chars, duplicate reserved headers)
 *    is performed by the C++ library — error codes map to the same negative
 *    sentinels documented in culpeo_wasm_api.h.
 *  - The input buffer is never mutated.
 */

#include "culpeo/c_api.h"
#include "culpeo/message.hpp"

#include <climits>
#include <cstdint>
#include <cstring>
#include <string_view>

extern "C" {

int culpeo_parse_headers(const uint8_t *buf, size_t len,
                         struct culpeo_header *headers_out, int max_headers,
                         size_t *body_offset_out) {
    if (buf == nullptr || headers_out == nullptr || body_offset_out == nullptr
        || max_headers <= 0) {
        return -2;
    }

    const auto frame = std::string_view(reinterpret_cast<const char *>(buf), len);

    // Use the C++ library for validation and header-block extraction.
    // FrameType::control covers both frame types for the purpose of header
    // parsing — only the body interpretation differs by frame type, and this
    // function does not interpret the body.
    const auto result =
        culpeo::message::parse_headers(culpeo::message::FrameType::control, frame);

    if (!result) {
        switch (result.error()) {
        case culpeo::message::Error::missing_header_terminator:
            return -1; // incomplete frame — caller should buffer more data
        case culpeo::message::Error::header_block_too_large:
            return -3; // header count / size limit exceeded
        default:
            return -2; // malformed (invalid line, duplicate header, …)
        }
    }

    const auto &parsed = *result;

    // body_offset is the byte distance from the start of the frame to the
    // first byte after \r\n\r\n.
    *body_offset_out =
        static_cast<size_t>(parsed.body.data() - frame.data());

    // Walk the raw header_block view to populate headers_out.
    // The C++ library has already validated every line, so we only need to
    // split on \r\n and locate the first ':'.
    const auto &hblock = parsed.header_block;
    int count = 0;
    size_t line_start = 0;

    while (line_start < hblock.size()) {
        if (count >= max_headers) {
            return -3; // caller-supplied capacity exhausted
        }

        // Locate end of line (\r\n or end of block).
        auto line_end = hblock.find("\r\n", line_start);
        if (line_end == std::string_view::npos) {
            line_end = hblock.size();
        }

        const auto line = hblock.substr(line_start, line_end - line_start);
        if (line.empty()) {
            line_start = (line_end < hblock.size()) ? line_end + 2 : line_end;
            continue;
        }

        const auto colon = line.find(':');
        if (colon == std::string_view::npos) {
            return -2; // should not happen — C++ already validated
        }

        auto key = line.substr(0, colon);
        // Trim trailing whitespace from key.
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
            key.remove_suffix(1);
        }

        auto val = line.substr(colon + 1);
        // Trim leading whitespace from value.
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) {
            val.remove_prefix(1);
        }
        // Trim trailing whitespace from value.
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) {
            val.remove_suffix(1);
        }

        // Store byte offsets relative to the start of buf.
        headers_out[count].key_ptr =
            static_cast<uint32_t>(key.data() - frame.data());
        headers_out[count].key_len = static_cast<uint32_t>(key.size());
        headers_out[count].val_ptr =
            static_cast<uint32_t>(val.data() - frame.data());
        headers_out[count].val_len = static_cast<uint32_t>(val.size());
        ++count;

        line_start = (line_end < hblock.size()) ? line_end + 2 : line_end;
    }

    return count;
}

int culpeo_serialize_frame(const struct culpeo_header *headers,
                           int header_count,
                           const uint8_t *strings_buf,
                           size_t strings_buf_len,
                           const uint8_t *body, size_t body_len,
                           uint8_t *out_buf, size_t out_cap) {
    // Finding 3 (part 1): guard against NULL body with non-zero body_len
    if (body_len > 0 && body == nullptr) {
        return -1;
    }
    if (out_buf == nullptr || out_cap == 0) {
        return -1;
    }
    if (header_count < 0) {
        return -1;
    }
    if (header_count > 0 && (headers == nullptr || strings_buf == nullptr)) {
        return -1;
    }

    // Build HeaderFieldView array pointing into strings_buf.
    // Stack-allocate up to 128 headers; heap-fallback is not needed given
    // the protocol limit of 64 headers (ParseLimits::max_header_count).
    constexpr int kMaxHeaders = 128;
    if (header_count > kMaxHeaders) {
        return -1;
    }

    culpeo::message::HeaderFieldView views[kMaxHeaders];
    for (int i = 0; i < header_count; ++i) {
        // Finding 3 (part 2): validate key/value offsets against strings_buf_len
        // to prevent heap over-reads from attacker-controlled offsets.
        const uint64_t key_end =
            static_cast<uint64_t>(headers[i].key_ptr) + headers[i].key_len;
        const uint64_t val_end =
            static_cast<uint64_t>(headers[i].val_ptr) + headers[i].val_len;
        if (key_end > strings_buf_len || val_end > strings_buf_len) {
            return -2; // out-of-bounds offset
        }
        views[i].name = std::string_view(
            reinterpret_cast<const char *>(strings_buf + headers[i].key_ptr),
            headers[i].key_len);
        views[i].value = std::string_view(
            reinterpret_cast<const char *>(strings_buf + headers[i].val_ptr),
            headers[i].val_len);
    }

    const auto hspan =
        std::span<const culpeo::message::HeaderFieldView>(views,
                                                          static_cast<size_t>(header_count));
    const auto bspan = std::span<const std::byte>(
        reinterpret_cast<const std::byte *>(body), body_len);
    auto ospan =
        std::span<std::byte>(reinterpret_cast<std::byte *>(out_buf), out_cap);

    const auto written = culpeo::message::serialize_frame_to_buffer(
        culpeo::message::FrameType::control, hspan, bspan, ospan);

    if (!written) {
        return -1; // buffer_too_small or invalid header chars
    }

    // Finding 7: guard against truncation when casting size_t → int.
    if (*written > static_cast<size_t>(INT_MAX)) {
        return -4; // output too large to represent as int
    }
    return static_cast<int>(*written);
}

} // extern "C"
