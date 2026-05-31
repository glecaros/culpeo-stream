# CS-P3-003: WebSocket Leaked If DisposeAsync Races With Reconnect Loop

**Date:** 2026-05-30  
**Severity:** Medium  
**Component:** C# — `CulpeoStreamClient.DisposeAsync`

## Description

`DisposeAsync` reads `_ws` and disposes it, but the reconnect loop can replace `_ws`
concurrently, causing the new WebSocket to go undisposed.

## Recommendation

In `DisposeAsync`, cancel the loop first, then await `_loopTask` to completion before
disposing `_ws` — by then no new WebSocket can be created.

## Status

Open
