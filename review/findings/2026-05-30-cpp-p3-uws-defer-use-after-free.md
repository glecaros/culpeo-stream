# CPP-P3-001: uWS Loop::defer() Called After Loop Shutdown — Use-After-Free

**Date:** 2026-05-30  
**Severity:** Critical  
**Component:** C++ — `libculpeo-transport-ws/include/culpeo/uws_adapter.hpp`

## Description

All three send/close callbacks capture a raw `uWS::Loop*` and call `loop->defer()`
unconditionally. If the uWS event loop unwinds (e.g., during shutdown) before the
Session destructor completes, `defer()` is called on a destroyed Loop — undefined
behaviour / crash.

The captured `uWS::Loop*` has no guarded lifetime. uWebSockets does not document
`Loop::defer()` as safe after loop teardown.

## Attack Scenario

1. WebSocket close handler fires → uWS begins loop shutdown
2. Loop object is destroyed
3. Session destructor (or a queued deferred callback) calls `loop->defer()`
4. Crash / memory corruption

## Recommendation

Add a `std::shared_ptr<std::atomic<bool>> alive` shutdown flag. Set it to `false` in
the `.close` handler before resetting the Session. Guard every `defer()` call
(pre-defer and inside the deferred lambda) with an `alive` load.

Also document required destruction order: Session MUST be destroyed before transport.

## Status

Open
