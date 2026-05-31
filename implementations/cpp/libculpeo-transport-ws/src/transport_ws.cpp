#include "culpeo/transport_ws.hpp"

#include <stdexcept>
#include <string>

namespace culpeo::transport {

WsTransport::WsTransport(SendFn send_text_fn, SendFn send_binary_fn, CloseFn close_fn)
    : send_text_fn_(std::move(send_text_fn))
    , send_binary_fn_(std::move(send_binary_fn))
    , close_fn_(std::move(close_fn)) {
    if (!send_text_fn_)   throw std::invalid_argument("WsTransport: send_text_fn must not be null");
    if (!send_binary_fn_) throw std::invalid_argument("WsTransport: send_binary_fn must not be null");
    if (!close_fn_)       throw std::invalid_argument("WsTransport: close_fn must not be null");
}

void WsTransport::send_text(std::span<const std::byte> frame) {
    std::lock_guard<std::mutex> lock(mu_);
    try { send_text_fn_(frame); } catch (...) { /* transport errors are non-fatal */ }
}

void WsTransport::send_binary(std::span<const std::byte> frame) {
    std::lock_guard<std::mutex> lock(mu_);
    try { send_binary_fn_(frame); } catch (...) { /* transport errors are non-fatal */ }
}

void WsTransport::close(int code, std::string_view reason) {
    // RFC 6455 §5.5.1: close frame payload = 2-byte status code + UTF-8 reason,
    // total ≤ 125 bytes → reason MUST be ≤ 123 bytes.
    if (reason.size() > 123) reason = reason.substr(0, 123);

    // Sanitize: replace ASCII control characters (< 0x20, except \t) with '?'.
    // This guards against \r\n injection and other non-UTF-8 control bytes.
    std::string safe_reason(reason);
    for (auto& c : safe_reason) {
        if (static_cast<unsigned char>(c) < 0x20 && c != '\t') c = '?';
    }

    std::lock_guard<std::mutex> lock(mu_);
    try { close_fn_(code, safe_reason); } catch (...) { /* close is best-effort */ }
}

}  // namespace culpeo::transport
