# [SEC-025] C++ H2 server sets no `SETTINGS_MAX_CONCURRENT_STREAMS` — unbounded stream creation exhausts memory

**Severity:** High  
**Component:** C++  
**Phase:** 4  
**Status:** Open  

## Description

`H2Session::init_nghttp2()` submits an empty HTTP/2 SETTINGS frame:

```cpp
// Submit initial SETTINGS
nghttp2_submit_settings(ng_, NGHTTP2_FLAG_NONE, nullptr, 0);
```

Because no `NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS` entry is included, nghttp2
advertises no limit.  Per RFC 9113 §6.5.2 the default when the setting is
omitted is "no limit" (effectively unlimited).

`H2Session::get_or_create_stream(int32_t stream_id)` unconditionally inserts a
new `StreamState` for every stream ID it has not seen before:

```cpp
H2Session::StreamState& H2Session::get_or_create_stream(int32_t stream_id)
{
    auto it = streams_.find(stream_id);
    if (it != streams_.end()) return it->second;

    StreamState& st = streams_[stream_id];
    st.channel = std::make_unique<FrameChannel>(strand_, kChannelCapacity);
    return st;
}
```

`FrameChannel` is an `asio::experimental::channel` with capacity 256 frames.
Each `StreamState` therefore allocates heap memory for the channel plus the
`RecvState` and `SendQueue` buffers.  HTTP/2 client stream IDs are 31-bit odd
integers; a single TCP connection can open up to 1,073,741,824 streams before
stream IDs are exhausted.

There is no cap anywhere in the server path that limits how many `StreamState`
objects accumulate in `streams_`.

## Impact

A single unauthenticated TCP connection can exhaust all server memory by
opening streams rapidly.  Each new HEADERS frame (one per stream) triggers
`on_begin_headers_callback` → `get_or_create_stream` → heap allocation.
nghttp2 will not refuse HEADERS frames that exceed a non-existent limit.
On a 64-bit server this will OOM-kill the process after a few thousand streams
depending on allocation size and overcommit settings.

Because the accept loop spawns sessions into `asio::detached`, each connection
creates an independent `H2Session`.  Multiple simultaneous attacking connections
multiply the effect.

This is also the primary amplification path for CVE-2023-44487-style rapid
reset attacks: open streams, reset them, repeat — nghttp2 frees the internal
stream state on RST_STREAM but the `StreamState` in `streams_` is only removed
by `on_stream_close_callback`, creating a window where memory is held for each
open-then-reset stream cycle.

## Location

`implementations/cpp/libculpeo-transport-h2/src/h2_client.cpp`:
- `H2Session::init_nghttp2()` — missing SETTINGS (line ~116)
- `H2Session::get_or_create_stream()` — no count check (line ~270)

## Recommendation

1. **Set `SETTINGS_MAX_CONCURRENT_STREAMS`** in `init_nghttp2()`:

```cpp
nghttp2_settings_entry settings[] = {
    { NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100 },
};
nghttp2_submit_settings(ng_, NGHTTP2_FLAG_NONE, settings,
                         sizeof(settings) / sizeof(settings[0]));
```

A reasonable production default is 100–1000; expose it as a configuration
parameter on `CulpeoH2Server`.

2. **Guard `get_or_create_stream` on the server side** with a cap matching the
SETTINGS value:

```cpp
if (mode_ == Mode::Server && streams_.size() >= max_concurrent_streams_) {
    nghttp2_submit_rst_stream(ng_, NGHTTP2_FLAG_NONE, stream_id,
                              NGHTTP2_REFUSED_STREAM);
    return sentinel_error_state;
}
```

Both mitigations are needed: the SETTINGS prevents well-behaved clients from
exceeding the limit, and the guard protects against clients that ignore
SETTINGS.
