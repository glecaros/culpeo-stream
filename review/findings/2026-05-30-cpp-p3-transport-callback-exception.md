# CPP-P3-002: std::function Callbacks Can Throw — Exception Escapes Transport

**Date:** 2026-05-30  
**Severity:** High  
**Component:** C++ — `libculpeo-transport-ws/src/transport_ws.cpp`

## Description

`WsTransport::send_text/send_binary/close` invoke `std::function` callbacks without
a try-catch. If a callback throws (e.g., allocation failure in the buffer copy in
`uws_adapter.hpp`), the exception propagates into the session layer which is not
uniformly prepared to handle it (some call sites catch, others don't).

## Recommendation

Wrap each callback invocation in try-catch:
```cpp
void WsTransport::send_text(std::span<const std::byte> frame) {
    std::lock_guard lock(mu_);
    try { send_text_fn_(frame); } catch (...) {}
}
```

## Status

Open
