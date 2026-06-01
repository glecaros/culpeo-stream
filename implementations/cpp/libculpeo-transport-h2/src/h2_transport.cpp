// h2_transport.cpp — H2Transport implementation
//
// H2Transport delegates all work to H2Session (the internal nghttp2 wrapper).
// This file contains only the thin public-API methods; the core logic is in
// h2_session.hpp / h2_client.cpp / h2_server.cpp.

#include "culpeo/h2_transport.hpp"
#include "h2_session.hpp"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/dispatch.hpp>
#include <asio/use_awaitable.hpp>

namespace culpeo::h2 {

H2Transport::H2Transport(std::shared_ptr<H2Session> session, int32_t stream_id)
    : session_(std::move(session)), stream_id_(stream_id)
{}

H2Transport::~H2Transport() = default;

asio::awaitable<void> H2Transport::send_text(std::span<const std::byte> frame)
{
    co_await session_->send_frame(stream_id_, kTypeControl, frame);
}

asio::awaitable<void> H2Transport::send_binary(std::span<const std::byte> frame)
{
    co_await session_->send_frame(stream_id_, kTypeMedia, frame);
}

asio::awaitable<void> H2Transport::close(int /*code*/, std::string_view /*reason*/)
{
    co_await session_->close_stream(stream_id_);
}

asio::awaitable<std::pair<uint8_t, std::vector<std::byte>>>
H2Transport::receive_frame()
{
    co_return co_await session_->receive_frame(stream_id_);
}

} // namespace culpeo::h2
