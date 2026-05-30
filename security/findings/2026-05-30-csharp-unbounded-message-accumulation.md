# SEC-009: Unbounded WebSocket Message Accumulation — Memory Exhaustion DoS

**Date:** 2026-05-30  
**Severity:** High  
**Component:** C# — `WebSocketTransportAdapter.ReceiveMessageAsync`

## Description

`ReceiveMessageAsync` accumulates WebSocket fragments into a `MemoryStream` with no size
limit. A client can stream a never-ending fragmented WebSocket message (FIN=0 forever)
and force the server to buffer arbitrarily large amounts of data before any protocol
parsing occurs — before the rate limiter can act.

## Attack Scenario

1. Attacker opens a WebSocket connection (passes HTTP upgrade).
2. Sends a fragmented message in 4 KB chunks, never setting FIN=1.
3. Server allocates ~4 KB per loop iteration indefinitely.
4. A few concurrent connections exhaust available heap.

## Recommendation

Add a configurable max message size (suggested default: 1 MB) and close with
`protocol-error` if exceeded:
```csharp
const int MaxMessageBytes = 1 * 1024 * 1024;
if (accumulator.Length + result.Count > MaxMessageBytes)
    throw new FormatException("Frame exceeds maximum allowed size.");
```
Expose via `CulpeoStreamOptions.MaxMessageBytes`.

## Status

Open
