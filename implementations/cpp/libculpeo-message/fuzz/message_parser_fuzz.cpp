#include "culpeo/message.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto bytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), size);

    auto control = culpeo::message::parse_headers(culpeo::message::FrameType::control, bytes);
    if (control && control->content_type.has_value()) {
        (void)culpeo::message::parse_content_type(*control->content_type);
    }

    auto media = culpeo::message::parse_headers(culpeo::message::FrameType::media, bytes);
    if (media && media->content_type.has_value()) {
        (void)culpeo::message::parse_content_type(*media->content_type);
    }

    return 0;
}
