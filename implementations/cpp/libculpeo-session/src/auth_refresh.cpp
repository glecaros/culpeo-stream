#include "auth_refresh.hpp"

#include <cstring>
#include <openssl/crypto.h>
#include <openssl/rand.h>

namespace culpeo::session::internal {

// ─── Helpers ──────────────────────────────────────────────────────────────────

// Hex-encode bytes to lowercase ASCII. Output must be at least 2*len bytes.
static void bytes_to_hex(const uint8_t* bytes, std::size_t len, char* out) noexcept {
    static constexpr char kHex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < len; ++i) {
        out[2 * i]     = kHex[(bytes[i] >> 4) & 0x0f];
        out[2 * i + 1] = kHex[bytes[i] & 0x0f];
    }
}

// Decode exactly kNonceBytes*2 hex chars into kNonceBytes bytes.
// Returns false on bad length or non-hex characters.
static bool hex_to_bytes(std::string_view hex,
                          std::array<uint8_t, kNonceBytes>& out) noexcept {
    if (hex.size() != kNonceBytes * 2) return false;

    for (std::size_t i = 0; i < kNonceBytes; ++i) {
        const char h = hex[2 * i];
        const char l = hex[2 * i + 1];

        const auto from_hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };

        const int hv = from_hex(h);
        const int lv = from_hex(l);
        if (hv < 0 || lv < 0) return false;

        out[i] = static_cast<uint8_t>((hv << 4) | lv);
    }
    return true;
}

// ─── AuthRefreshManager ───────────────────────────────────────────────────────

AuthRefreshManager::~AuthRefreshManager() noexcept {
    clear();
}

std::expected<std::string, Error>
AuthRefreshManager::generate(uint32_t min_interval_s) noexcept {
    if (pending_) {
        return std::unexpected(Error::nonce_already_pending);
    }

    // Enforce minimum re-issue interval
    const auto now = std::chrono::steady_clock::now();
    if (last_issued_.time_since_epoch().count() != 0) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_issued_);
        if (elapsed.count() < static_cast<long>(min_interval_s)) {
            return std::unexpected(Error::nonce_already_pending);
        }
    }

    // Fill nonce buffer with CSPRNG bytes
    if (RAND_bytes(nonce_.data(), static_cast<int>(kNonceBytes)) != 1) {
        return std::unexpected(Error::transport_error);
    }

    // Hex-encode the nonce for transport
    std::string hex_nonce(kNonceBytes * 2, '\0');
    bytes_to_hex(nonce_.data(), kNonceBytes, hex_nonce.data());

    pending_ = true;
    issued_at_ = now;
    last_issued_ = now;

    return hex_nonce;
}

std::expected<void, Error>
AuthRefreshManager::validate_and_consume(std::string_view echoed_hex,
                                          uint32_t timeout_s) noexcept {
    if (!pending_) {
        return std::unexpected(Error::nonce_mismatch);
    }

    if (is_expired(timeout_s)) {
        clear();
        return std::unexpected(Error::nonce_expired);
    }

    // Decode echoed hex
    std::array<uint8_t, kNonceBytes> echoed_bytes{};
    if (!hex_to_bytes(echoed_hex, echoed_bytes)) {
        clear();
        return std::unexpected(Error::nonce_mismatch);
    }

    // Constant-time comparison to prevent timing oracle attacks
    const int eq = CRYPTO_memcmp(echoed_bytes.data(), nonce_.data(), kNonceBytes);

    // Zero both buffers immediately after comparison, regardless of result
    OPENSSL_cleanse(echoed_bytes.data(), kNonceBytes);
    clear();

    if (eq != 0) {
        return std::unexpected(Error::nonce_mismatch);
    }

    return {};
}

bool AuthRefreshManager::is_expired(uint32_t timeout_s) const noexcept {
    if (!pending_) return false;
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - issued_at_);
    return elapsed.count() >= static_cast<long>(timeout_s);
}

void AuthRefreshManager::clear() noexcept {
    // OPENSSL_cleanse performs a secure zero that the compiler cannot optimize away
    OPENSSL_cleanse(nonce_.data(), kNonceBytes);
    pending_ = false;
    // Keep last_issued_ so the rate limiter still works after a clear
}

}  // namespace culpeo::session::internal
