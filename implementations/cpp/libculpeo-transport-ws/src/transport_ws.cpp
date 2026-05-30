#include "culpeo/transport_ws.hpp"

#include <stdexcept>

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
    send_text_fn_(frame);
}

void WsTransport::send_binary(std::span<const std::byte> frame) {
    std::lock_guard<std::mutex> lock(mu_);
    send_binary_fn_(frame);
}

void WsTransport::close(int code, std::string_view reason) {
    std::lock_guard<std::mutex> lock(mu_);
    close_fn_(code, reason);
}

}  // namespace culpeo::transport
