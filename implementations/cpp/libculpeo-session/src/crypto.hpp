#pragma once

// Platform-abstracted cryptographic primitives for libculpeo-session.
//
// Provides:
//   secure_random()       — fill a buffer from a CSPRNG-backed std::random_device
//   secure_zero()         — zero memory in a way the compiler cannot elide
//   constant_time_equal() — timing-safe byte comparison
//
// CSPRNG backing by platform:
//   libstdc++ (Linux/Android): /dev/urandom
//   libc++ (macOS/iOS):        /dev/urandom
//   MSVC (Windows):            BCryptGenRandom
//   Emscripten (WASM):         crypto.getRandomValues via emscripten_get_entropy
//
// Unsupported platforms fail at compile time via static_assert.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <span>
#include <stdexcept>

#if defined(__EMSCRIPTEN__)
#  include <emscripten.h>
#elif defined(__GLIBC__) || defined(__APPLE__)
#  include <strings.h>      // explicit_bzero()
#endif

// ─── Platform allowlist ──────────────────────────────────────────────────────
// Verify that std::random_device is backed by a real CSPRNG on this platform.

#if !defined(__linux__) && !defined(__APPLE__) && !defined(_WIN32) && !defined(__EMSCRIPTEN__)
static_assert(false,
    "culpeo::crypto: unverified platform — confirm std::random_device is CSPRNG-backed");
#endif

namespace culpeo::crypto {

// Fill `buf` with cryptographically secure random bytes.
// Throws std::runtime_error if the entropy source is unavailable.
inline void secure_random(std::span<std::byte> buf) {
    if (buf.empty()) return;

#if defined(__EMSCRIPTEN__)
    emscripten_get_entropy(buf.data(), buf.size());
#else
    thread_local std::random_device rd;

    auto* p = reinterpret_cast<uint8_t*>(buf.data());
    std::size_t remaining = buf.size();

    while (remaining >= sizeof(std::random_device::result_type)) {
        auto val = rd();
        std::memcpy(p, &val, sizeof(val));
        p += sizeof(val);
        remaining -= sizeof(val);
    }

    if (remaining > 0) {
        auto val = rd();
        std::memcpy(p, &val, remaining);
    }
#endif
}

// Zero memory in a way the compiler cannot optimize away.
inline void secure_zero(void* ptr, std::size_t len) noexcept {
    if (!ptr || len == 0) return;

#if defined(_WIN32)
    SecureZeroMemory(ptr, len);
#elif defined(__GLIBC__) || defined(__APPLE__)
    explicit_bzero(ptr, len);
#else
    volatile unsigned char* p = static_cast<volatile unsigned char*>(ptr);
    while (len--) *p++ = 0;
#endif
}

// Convenience overload for span.
inline void secure_zero(std::span<std::byte> buf) noexcept {
    secure_zero(buf.data(), buf.size());
}

// Constant-time equality comparison. Returns true iff all bytes are equal.
// Length is not considered secret — returns false immediately on mismatch.
[[nodiscard]] inline bool constant_time_equal(
        std::span<const uint8_t> a,
        std::span<const uint8_t> b) noexcept {
    if (a.size() != b.size()) return false;
    uint8_t diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    }
    return diff == 0;
}

}  // namespace culpeo::crypto
