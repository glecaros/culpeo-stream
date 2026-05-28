#include "stream_registry.hpp"

#include "culpeo/frame.hpp"

#include <algorithm>
#include <array>
#include <cstring>

// OpenSSL for CSPRNG
#include <openssl/rand.h>

namespace culpeo::session::internal {

// ─── Helpers ──────────────────────────────────────────────────────────────────

[[nodiscard]] std::optional<StreamDirection> parse_stream_direction(std::string_view s) noexcept {
    if (s == "input") return StreamDirection::input;
    if (s == "output") return StreamDirection::output;
    if (s == "duplex") return StreamDirection::duplex;
    return std::nullopt;
}

[[nodiscard]] std::string_view stream_direction_to_string(StreamDirection dir) noexcept {
    switch (dir) {
    case StreamDirection::input:  return "input";
    case StreamDirection::output: return "output";
    case StreamDirection::duplex: return "duplex";
    }
    return "unknown";
}

[[nodiscard]] StreamCodec codec_from_content_type(std::string_view ct) noexcept {
    auto parsed = culpeo::frame::parse_content_type(ct);
    if (!parsed) return StreamCodec::other;

    return std::visit([](const auto& v) -> StreamCodec {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, culpeo::frame::AudioPcmContentType>)  return StreamCodec::pcm;
        if constexpr (std::is_same_v<T, culpeo::frame::AudioOpusContentType>) return StreamCodec::opus;
        if constexpr (std::is_same_v<T, culpeo::frame::AudioAacContentType>)  return StreamCodec::aac;
        return StreamCodec::other;
    }, *parsed);
}

[[nodiscard]] std::expected<std::string, Error>
generate_csprng_id(std::size_t len_bytes) noexcept {
    // Buffer capped at 32 bytes (256 bits) — more than enough for any ID
    std::array<uint8_t, 32> buf{};
    if (len_bytes > buf.size()) len_bytes = buf.size();

    if (RAND_bytes(buf.data(), static_cast<int>(len_bytes)) != 1) {
        return std::unexpected(Error::transport_error);  // OpenSSL CSPRNG failure
    }

    static constexpr char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(len_bytes * 2);
    for (std::size_t i = 0; i < len_bytes; ++i) {
        result += hex_chars[(buf[i] >> 4) & 0x0f];
        result += hex_chars[buf[i] & 0x0f];
    }

    // Zero the working buffer immediately after encoding
    // (OPENSSL_cleanse is overkill for temporary IDs, but safe)
    std::memset(buf.data(), 0, len_bytes);

    return result;
}

// ─── Validation ──────────────────────────────────────────────────────────────

[[nodiscard]] std::expected<void, Error>
validate_declarations(const std::vector<StreamDeclaration>& decls, uint32_t max_streams) noexcept {
    // Rule 1: At least one stream declared
    if (decls.empty()) {
        return std::unexpected(Error::invalid_streams);
    }

    // Rule 2/6: max_streams limit (spec §5.6)
    if (decls.size() > static_cast<std::size_t>(max_streams)) {
        return std::unexpected(Error::max_streams_exceeded);
    }

    for (const auto& decl : decls) {
        // Rule 2: content_type and type are required (empty strings → invalid)
        if (decl.content_type.empty()) {
            return std::unexpected(Error::invalid_streams);
        }
        // Rule 3: direction must be a valid value (always true for typed enum,
        // but if parsing failed and caller passed a bad value we'd catch it here)
    }

    // Rule 4/5: purpose uniqueness within type
    // Count streams per direction; if count > 1, all must have non-empty purpose
    // and purpose must be unique within that direction.
    for (auto dir : {StreamDirection::input, StreamDirection::output, StreamDirection::duplex}) {
        std::vector<std::string_view> purposes;
        std::size_t count = 0;

        for (const auto& decl : decls) {
            if (decl.direction != dir) continue;
            ++count;
            purposes.push_back(decl.purpose);
        }

        if (count <= 1) continue;

        // Two or more streams of the same type: all must have purpose
        for (auto& p : purposes) {
            if (p.empty()) {
                return std::unexpected(Error::invalid_streams);
            }
        }

        // purposes must be unique
        for (std::size_t i = 0; i < purposes.size(); ++i) {
            for (std::size_t j = i + 1; j < purposes.size(); ++j) {
                if (purposes[i] == purposes[j]) {
                    return std::unexpected(Error::invalid_streams);
                }
            }
        }
    }

    return {};
}

// ─── StreamRegistry ──────────────────────────────────────────────────────────

StreamRegistry::StreamRegistry(uint32_t max_streams) noexcept
    : max_streams_(max_streams) {}

std::expected<void, Error>
StreamRegistry::register_from_declarations(const std::vector<StreamDeclaration>& decls) noexcept {
    streams_.clear();

    for (const auto& decl : decls) {
        auto id_result = generate_csprng_id(8);  // 8 bytes = 64 bits of entropy
        if (!id_result) return std::unexpected(id_result.error());

        StreamInfo info{};
        info.id = std::move(*id_result);
        info.content_type = decl.content_type;
        info.direction = decl.direction;
        info.purpose = decl.purpose;
        info.offset = 0;
        info.codec = codec_from_content_type(decl.content_type);

        // Extract PCM params if applicable
        auto ct_parsed = culpeo::frame::parse_content_type(decl.content_type);
        if (ct_parsed) {
            if (const auto* pcm =
                    std::get_if<culpeo::frame::AudioPcmContentType>(&*ct_parsed)) {
                info.pcm_params = PcmParams{.rate = pcm->rate,
                                            .channels = pcm->channels,
                                            .bits = pcm->bits};
            }
        }

        streams_.emplace(info.id, std::move(info));
    }

    return {};
}

void StreamRegistry::register_from_persisted(const std::vector<StreamInfo>& persisted) noexcept {
    streams_.clear();
    for (const auto& s : persisted) {
        streams_.emplace(s.id, s);
    }
}

const StreamInfo* StreamRegistry::find(std::string_view id) const noexcept {
    auto it = streams_.find(std::string(id));
    if (it == streams_.end()) return nullptr;
    return &it->second;
}

StreamInfo* StreamRegistry::find_mutable(std::string_view id) noexcept {
    auto it = streams_.find(std::string(id));
    if (it == streams_.end()) return nullptr;
    return &it->second;
}

std::expected<void, Error>
StreamRegistry::validate_server_send(std::string_view stream_id) const noexcept {
    const auto* s = find(stream_id);
    if (!s) return std::unexpected(Error::stream_not_found);
    if (s->direction == StreamDirection::input) {
        return std::unexpected(Error::invalid_direction);
    }
    return {};
}

std::expected<void, Error>
StreamRegistry::validate_client_send(std::string_view stream_id) const noexcept {
    const auto* s = find(stream_id);
    if (!s) return std::unexpected(Error::stream_not_found);
    if (s->direction == StreamDirection::output) {
        return std::unexpected(Error::invalid_direction);
    }
    return {};
}

std::vector<StreamInfo> StreamRegistry::snapshot() const noexcept {
    std::vector<StreamInfo> result;
    result.reserve(streams_.size());
    for (const auto& [_, info] : streams_) {
        result.push_back(info);
    }
    return result;
}

void StreamRegistry::clear() noexcept {
    streams_.clear();
}

}  // namespace culpeo::session::internal
