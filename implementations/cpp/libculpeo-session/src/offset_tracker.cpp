#include "offset_tracker.hpp"

#include <limits>

namespace culpeo::session::internal {

std::expected<uint64_t, Error>
compute_pcm_increment(uint64_t frame_bytes, const PcmParams& params) noexcept {
    if (params.channels == 0 || params.bits == 0) {
        return std::unexpected(Error::offset_overflow);  // Guard: invalid PCM params
    }
    if (params.bits % 8 != 0) {
        return std::unexpected(Error::offset_overflow);  // Bits must be a multiple of 8
    }

    const uint32_t bytes_per_sample_chan = params.bits / 8u;

    // Overflow check: channels * bytes_per_sample_chan
    // Both are uint16/uint32; product fits in uint64 easily, but let's be explicit.
    const uint64_t bytes_per_frame_sample =
        static_cast<uint64_t>(params.channels) * static_cast<uint64_t>(bytes_per_sample_chan);

    if (bytes_per_frame_sample == 0) {
        return std::unexpected(Error::offset_overflow);
    }

    // Integer division: samples = floor(frame_bytes / bytes_per_frame_sample)
    const uint64_t increment = frame_bytes / bytes_per_frame_sample;

    return increment;
}

std::expected<void, Error>
advance_offset(StreamInfo& stream, uint64_t frame_bytes) noexcept {
    uint64_t increment = 1;

    if (stream.codec == StreamCodec::pcm) {
        if (!stream.pcm_params.has_value()) {
            return std::unexpected(Error::offset_overflow);  // Missing PCM metadata
        }
        auto inc = compute_pcm_increment(frame_bytes, *stream.pcm_params);
        if (!inc) return std::unexpected(inc.error());
        increment = *inc;
    }
    // For encoded streams (opus, aac, other): increment by 1 per frame (spec §8.2)

    // Overflow check on offset addition
    if (increment > std::numeric_limits<uint64_t>::max() - stream.offset) {
        return std::unexpected(Error::offset_overflow);
    }

    stream.offset += increment;
    return {};
}

std::expected<void, Error>
check_offset(const StreamInfo& stream, uint64_t frame_offset) noexcept {
    if (frame_offset != stream.offset) {
        return std::unexpected(Error::offset_mismatch);
    }
    return {};
}

}  // namespace culpeo::session::internal
