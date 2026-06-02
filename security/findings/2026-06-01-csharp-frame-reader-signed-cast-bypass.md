# [SEC-024] C# `Http2FrameReader`: signed cast truncation bypasses max-payload guard and throws unhandled exception

**Severity:** High  
**Component:** C#  
**Phase:** 4  
**Status:** Open  

## Description

`Http2FrameReader.ReadFrameAsync` reads the 4-byte big-endian length field and
immediately casts the result from `uint` to `int` before the bounds check:

```csharp
var payloadLength = (int)BinaryPrimitives.ReadUInt32BigEndian(header.AsSpan(1));

if (payloadLength > maxPayloadBytes)   // signed comparison
{
    throw new CulpeoProtocolException("frame-too-large", …);
}

var payload = new byte[payloadLength]; // allocates with signed value
```

`BinaryPrimitives.ReadUInt32BigEndian` returns `uint`.  For any wire value
≥ `0x80000000` (bit 31 set) the unchecked `(int)` cast yields a **negative**
`int`.  Because both sides of `>` are `int`, the comparison becomes
`negative_value > 16_777_216`, which is always `false` — the check is silently
bypassed.

The subsequent `new byte[payloadLength]` receives a negative array length and
throws `OverflowException` (in typical .NET runtimes).  This exception is not
caught anywhere in `ReadFrameAsync`, `ReceiveFrameAsync`, or the endpoint
handler's outer `try/catch` (which only catches `OperationCanceledException`).
The unhandled exception terminates the per-connection handler task.

## Impact

A remote client (or a server, from the client's perspective) can send a frame
with a length field whose MSB is set (e.g., `0x80000001`).  The server
connection handler crashes immediately on receipt of that frame.  Because each
HTTP/2 stream is independent but handled within the same `HttpContext` task,
this terminates the connection for the attacking client only, but it does so
silently and bypasses the declared maximum-payload protection.

Against a production server where many clients connect, an attacker with the
ability to open connections (even unauthenticated at the HTTP layer) can
reliably and cheaply crash individual connections.  Any connection that has not
yet authenticated can be torn down this way before the `culpeo.init` frame is
processed.

## Location

`implementations/csharp/src/CulpeoStream.Http2/Http2FrameReader.cs`, line ~35
(`ReadFrameAsync`, the `(int)` cast and subsequent bounds check).

## Recommendation

Read the length as `uint`, validate it as `uint`, and only convert to `int`
after the check — or keep it as `uint` throughout and use an unsigned
comparison:

```csharp
var rawLength = BinaryPrimitives.ReadUInt32BigEndian(header.AsSpan(1));

if (rawLength > (uint)maxPayloadBytes)
{
    throw new CulpeoProtocolException(
        "frame-too-large",
        $"Frame payload length {rawLength} bytes exceeds the configured maximum of {maxPayloadBytes} bytes.");
}

var payloadLength = (int)rawLength; // safe: rawLength ≤ maxPayloadBytes ≤ int.MaxValue
var payload = new byte[payloadLength];
```

This eliminates the signed-overflow path entirely.  The default
`DefaultMaxPayloadBytes` is well within `int.MaxValue` so the final cast is
safe after the guard.
