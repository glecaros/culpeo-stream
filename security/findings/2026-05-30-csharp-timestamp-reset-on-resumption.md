# CS-001: Timestamp Resets to Zero on Session Resumption (C#)

**Date:** 2026-05-30  
**Severity:** High  
**Component:** C# — `CulpeoStream.AspNetCore` / `WebSocketTransportAdapter`

## Description

When a session is resumed after reconnection, `_sessionStart` is set to `DateTimeOffset.UtcNow` at the moment of reconnection rather than the original session establishment time. This causes all timestamps on media frames after resumption to restart from zero, violating §8.3 of the spec:

> "The `Timestamp` header carries the presentation timestamp in **microseconds since session start** (since `culpeo.init-ack` was first sent by the server)."

Receivers tracking presentation time across reconnections (e.g., audio synchronization) will observe a discontinuity.

## Location

- `src/CulpeoStream.AspNetCore/Internal/WebSocketTransportAdapter.cs`, line 197 (`_sessionStart = DateTimeOffset.UtcNow`)
- `_sessionStart` is never populated from `SessionSnapshot`

## Recommendation

Add `DateTimeOffset SessionStartedAt` to `SessionSnapshot` (set once, at new session creation only). On resumption, initialize `_sessionStart` from the snapshot value:

```csharp
// SessionSnapshot:
public DateTimeOffset SessionStartedAt { get; init; }

// WebSocketTransportAdapter.RunAsync():
_sessionStart = _connection.SessionSnapshot.SessionStartedAt;
```

## Status

Open
