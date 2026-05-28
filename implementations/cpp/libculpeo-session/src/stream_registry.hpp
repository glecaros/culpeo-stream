#pragma once

// Internal stream registry — not part of the public API.
// This header is only included by session.cpp, stream_registry.cpp, and tests of
// stream_registry internals.

#include "culpeo/session.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace culpeo::session::internal {

// Validate stream declarations per Section 5.5 and 5.6.
// Returns Error::max_streams_exceeded or Error::invalid_streams on failure.
[[nodiscard]] std::expected<void, Error>
validate_declarations(const std::vector<StreamDeclaration>& decls, uint32_t max_streams) noexcept;

// Parse StreamDirection from the "type" field string.
[[nodiscard]] std::optional<StreamDirection> parse_stream_direction(std::string_view s) noexcept;

// Return the canonical string for a StreamDirection.
[[nodiscard]] std::string_view stream_direction_to_string(StreamDirection dir) noexcept;

// Determine codec from content_type string (uses culpeo::frame::parse_content_type).
[[nodiscard]] StreamCodec codec_from_content_type(std::string_view ct) noexcept;

// Generate a CSPRNG-backed opaque identifier, hex-encoded.
// len_bytes: bytes of entropy (e.g. 16 for session IDs, 8 for stream IDs).
[[nodiscard]] std::expected<std::string, Error> generate_csprng_id(std::size_t len_bytes) noexcept;

// Manages the set of declared streams for one session.
class StreamRegistry {
public:
    explicit StreamRegistry(uint32_t max_streams) noexcept;

    // Register streams from validated declarations (fresh session).
    // Assigns server-generated IDs. On success, streams are queryable immediately.
    [[nodiscard]] std::expected<void, Error>
    register_from_declarations(const std::vector<StreamDeclaration>& decls) noexcept;

    // Restore streams from persisted state (resumption path).
    void register_from_persisted(const std::vector<StreamInfo>& persisted) noexcept;

    // Lookup by server-assigned stream ID.
    [[nodiscard]] const StreamInfo* find(std::string_view id) const noexcept;
    [[nodiscard]] StreamInfo* find_mutable(std::string_view id) noexcept;

    // Validate that the server may send on this stream (direction: output or duplex).
    [[nodiscard]] std::expected<void, Error>
    validate_server_send(std::string_view stream_id) const noexcept;

    // Validate that the client may send on this stream (direction: input or duplex).
    [[nodiscard]] std::expected<void, Error>
    validate_client_send(std::string_view stream_id) const noexcept;

    // Return all registered streams as a snapshot vector.
    [[nodiscard]] std::vector<StreamInfo> snapshot() const noexcept;

    // Clear all streams (e.g., on session close).
    void clear() noexcept;

    [[nodiscard]] bool empty() const noexcept { return streams_.empty(); }

private:
    uint32_t max_streams_;
    std::unordered_map<std::string, StreamInfo> streams_;
};

}  // namespace culpeo::session::internal
