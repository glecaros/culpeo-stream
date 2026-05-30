#pragma once

// Internal offset tracker — not part of the public API.

#include "culpeo/session.hpp"

#include <cstdint>
#include <expected>
#include <limits>

namespace culpeo::session::internal {

// Compute the per-frame offset increment for a PCM stream.
//
// increment = frame_bytes / (channels * (bits / 8))
//
// Returns Error::offset_overflow if the intermediate multiplication overflows,
// if bits is zero, if channels is zero, or if bytes_per_sample is zero (cannot divide).
[[nodiscard]] std::expected<uint64_t, Error>
compute_pcm_increment(uint64_t frame_bytes, const PcmParams& params) noexcept;

// Advance a stream's offset by the appropriate increment for the stream's offset_type.
// For OffsetType::time    (PCM): increment = compute_pcm_increment(frame_bytes, *stream.pcm_params)
// For OffsetType::byte           : increment = frame_bytes
// For OffsetType::message        : increment = 1
//
// The legacy stream.codec field is NOT used to determine the increment; it is retained
// only for PCM parameter extraction (pcm_params population).
//
// Returns Error::offset_overflow if the new offset would exceed UINT64_MAX.
// Returns Error::offset_overflow if PCM increment computation overflows.
[[nodiscard]] std::expected<void, Error>
advance_offset(StreamInfo& stream, uint64_t frame_bytes) noexcept;

// Validate that a frame's offset header matches the stream's next expected offset.
// Returns Error::offset_mismatch if they differ.
[[nodiscard]] std::expected<void, Error>
check_offset(const StreamInfo& stream, uint64_t frame_offset) noexcept;

}  // namespace culpeo::session::internal
