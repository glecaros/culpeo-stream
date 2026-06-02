# [SEC-033] C++ `encode_h2_envelope`: no send-side payload size check; payload > UINT32_MAX−1 silently corrupts length prefix

**Severity:** Low  
**Component:** C++  
**Phase:** 4  
**Status:** Open  

## Description

`encode_h2_envelope` in `h2_session.hpp` computes the 4-byte length field as:

```cpp
inline std::vector<uint8_t> encode_h2_envelope(uint8_t type_byte,
                                                std::span<const std::byte> payload)
{
    uint32_t len = 1u + static_cast<uint32_t>(payload.size());
    // …
}
```

On a 64-bit system, `payload.size()` is a `std::size_t` (64-bit).  The cast to
`uint32_t` truncates silently.  If `payload.size()` is ≥ `0xFFFFFFFF`
(4,294,967,295 bytes ≈ 4 GiB), the addition `1u + (uint32_t)payload.size()`
wraps to a small value (e.g., `payload.size() = 0xFFFFFFFF` → `len = 0`).

`H2Session::send_frame` does not check `payload.size()` before calling
`encode_h2_envelope`:

```cpp
asio::awaitable<void> H2Session::send_frame(int32_t stream_id,
                                             uint8_t type_byte,
                                             std::span<const std::byte> payload)
{
    // … no size check …
    auto envelope = encode_h2_envelope(type_byte, payload);
    enqueue_send(stream_id, std::move(envelope));
    // …
}
```

The resulting envelope will have a corrupt (too-small) length prefix but will
contain the full multi-GiB payload bytes, causing the receiver to read a tiny
number of bytes, interpret them as the next frame, and then desync the framing
for all subsequent frames on the stream.

## Impact

This is a **sender-side** bug rather than a remote-exploitation vector.
Exploitation requires the application layer to call `send_frame` with a payload
larger than ~4 GiB, which is unlikely in practice given that `kMaxFrameSize`
is enforced on the receiver side.  However:

1. The send side has no corresponding size limit — there is no assertion or
   error return in `send_frame` for oversized payloads.

2. A stream that encounters this will silently corrupt its framing for all
   subsequent frames, which could be misused if an attacker controls the size
   of a media payload (e.g., through a specially crafted audio stream that
   causes a buffer accumulation path to produce an oversized payload buffer).

3. The absence of the check is inconsistent with the receiver's `kMaxFrameSize`
   enforcement, creating an asymmetric trust boundary.

## Location

`implementations/cpp/libculpeo-transport-h2/src/h2_session.hpp`,
`encode_h2_envelope` function; and
`implementations/cpp/libculpeo-transport-h2/src/h2_client.cpp`,
`H2Session::send_frame`.

## Recommendation

Add an explicit size check in `send_frame` before calling `encode_h2_envelope`:

```cpp
asio::awaitable<void> H2Session::send_frame(int32_t stream_id,
                                             uint8_t type_byte,
                                             std::span<const std::byte> payload)
{
    if (payload.size() > kMaxFrameSize - 1) {  // -1 for type byte
        throw std::invalid_argument(
            "send_frame: payload exceeds kMaxFrameSize");
    }
    // …
}
```

Alternatively, assert in `encode_h2_envelope` itself:

```cpp
uint32_t len = 1u + static_cast<uint32_t>(payload.size());
assert(payload.size() <= std::numeric_limits<uint32_t>::max() - 1);
```

Either approach catches the overflow at the point of misuse rather than
allowing a silently corrupted envelope to reach the wire.
