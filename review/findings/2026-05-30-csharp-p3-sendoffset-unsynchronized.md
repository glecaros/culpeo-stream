# CS-P3-004: SendOffset Mutated From User Threads Without Synchronization

**Date:** 2026-05-30  
**Severity:** Medium  
**Component:** C# — `CulpeoStreamClient.SendMediaAsync`

## Description

`SendMediaAsync` reads and increments `stream.SendOffset` from the caller thread, while
the receive loop writes `ReceiveOffset` on a different thread, with no locking on the
`ClientStreamState` object. The `_sendLock` serializes WebSocket writes but not the
offset arithmetic, so concurrent `SendMediaAsync` calls can race on offset updates.

## Recommendation

Use `Interlocked.Add` for `SendOffset` increments, or hold `_sendLock` across the
offset read-increment-write too.

## Status

Open
