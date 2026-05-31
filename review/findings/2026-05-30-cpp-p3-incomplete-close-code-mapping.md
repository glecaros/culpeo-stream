# CPP-P3-003: Missing RFC 6455 Mappings for server-shutdown and idle-timeout

**Date:** 2026-05-30  
**Severity:** Medium  
**Component:** C++ — `libculpeo-session/src/session.cpp` `to_ws_close_code()`

## Description

`server-shutdown` and `idle-timeout` fall through to 1002 (Protocol Error). Per
RFC 6455 §7.4.1 they should map to 1001 (Going Away), which better reflects that the
endpoint is intentionally shutting down rather than violating the protocol.

## Recommendation

```cpp
if (culpeo_code == "server-shutdown" || culpeo_code == "idle-timeout")
    return 1001;
```

## Status

Open
