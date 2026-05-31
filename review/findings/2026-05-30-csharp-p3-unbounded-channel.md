# CS-P3-002: Unbounded Event Channel — Memory Exhaustion if Consumer Is Slow

**Date:** 2026-05-30  
**Severity:** High  
**Component:** C# — `CulpeoStreamClient._eventChannel`

## Description

`Channel.CreateUnbounded<CulpeoClientEvent>()` with no capacity limit. High-frequency
media streams with a slow `ReceiveAsync` consumer will grow the channel without bound.

## Recommendation

Switch to `Channel.CreateBounded<CulpeoClientEvent>(new BoundedChannelOptions(capacity)
{ FullMode = BoundedChannelFullMode.Wait })` and document the backpressure contract,
or document clearly that callers must consume continuously.

## Status

Open
