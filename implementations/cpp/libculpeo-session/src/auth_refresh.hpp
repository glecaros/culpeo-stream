#pragma once

// Internal auth-refresh nonce manager — not part of the public API.
//
// Security properties:
//  - Nonces are 32 bytes (256 bits) from RAND_bytes (OpenSSL CSPRNG).
//  - Stored bytes are zeroed via OPENSSL_cleanse after use.
//  - Comparison uses CRYPTO_memcmp (constant-time) to prevent timing attacks.
//  - Only one outstanding nonce per session at a time.
//  - Minimum re-issue interval is enforced to prevent challenge flooding.

#include "culpeo/session.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace culpeo::session::internal {

inline constexpr std::size_t kNonceBytes = 32;  // 256-bit nonce

class AuthRefreshManager {
public:
    AuthRefreshManager() noexcept = default;
    ~AuthRefreshManager() noexcept;

    // Non-copyable (holds sensitive key material)
    AuthRefreshManager(const AuthRefreshManager&) = delete;
    AuthRefreshManager& operator=(const AuthRefreshManager&) = delete;

    // Generate a new nonce and return it as a lowercase hex string (64 chars).
    //
    // Fails with Error::nonce_already_pending if a nonce is already outstanding.
    // Fails with Error::transport_error if RAND_bytes fails (extremely rare).
    // Enforces min_interval_s: fails if last challenge was issued too recently.
    [[nodiscard]] std::expected<std::string, Error>
    generate(uint32_t min_interval_s) noexcept;

    // Validate echoed_hex against the stored nonce using constant-time comparison.
    // Clears (zeroes) the stored nonce on success.
    //
    // Returns Error::nonce_mismatch if the value doesn't match or no nonce is pending.
    // Returns Error::nonce_expired if the nonce has timed out.
    [[nodiscard]] std::expected<void, Error>
    validate_and_consume(std::string_view echoed_hex, uint32_t timeout_s) noexcept;

    [[nodiscard]] bool is_pending() const noexcept { return pending_; }

    // True if the outstanding nonce is older than timeout_s.
    [[nodiscard]] bool is_expired(uint32_t timeout_s) const noexcept;

    // Zero nonce buffer and clear all state.
    void clear() noexcept;

private:
    // Sensitive: must be zeroed via OPENSSL_cleanse, not memset.
    alignas(std::size_t) std::array<uint8_t, kNonceBytes> nonce_{};

    bool pending_{false};
    std::chrono::steady_clock::time_point issued_at_{};
    std::chrono::steady_clock::time_point last_issued_{};  // For rate limiting
};

}  // namespace culpeo::session::internal
