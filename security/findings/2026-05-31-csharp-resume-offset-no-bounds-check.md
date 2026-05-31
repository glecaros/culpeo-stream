## Finding: SEC-019 — Server-Supplied resume_offset Trusted Without Bounds Validation
**Severity:** Medium
**Target:** C#
**Phase:** Phase 3 — `CulpeoStream.Client`

### Description

`CulpeoStreamClient.ProcessInitAck()` reads `resume_offset` from the server's
`culpeo.init-ack` body and applies it directly to the client's stream-offset cursors
without any bounds checking:

```csharp
if (isResumption && streamEl.TryGetProperty("resume_offset", out var roProp))
{
    var confirmedOffset = roProp.GetInt64();   // no range validation
    if (streamType is CulpeoStreamType.Input or CulpeoStreamType.Duplex)
        state.SendOffset = confirmedOffset;    // ← send cursor reset to server value
    if (streamType is CulpeoStreamType.Output or CulpeoStreamType.Duplex)
        state.ReceiveOffset = confirmedOffset; // ← receive cursor reset to server value
}
```

The following invalid values are all silently accepted:

| Value | Effect |
|---|---|
| `confirmedOffset < 0` | Negative offset stored; subsequent arithmetic (`offset + increment`) produces garbage or unexpected frame numbering. |
| `confirmedOffset = 0` | Client's send cursor reset to zero — all previously sent data is retransmitted, causing duplicate delivery to the application layer. |
| `confirmedOffset >> client's actual offset` | Client numbers future frames with an inflated offset; a legitimate server that enforces monotonic offsets will reject every subsequent media frame. |
| `Int64.MaxValue` | The first `SendOffset += increment` overflows to a negative value (no overflow protection in `ComputeOffsetIncrement` path). |

### Attack Scenario (Rogue or MITM Server)

This attack is reachable when:
- `AllowInsecureConnections = true` (ws://) is set and a network attacker performs a
  man-in-the-middle.
- A server process is compromised and begins returning manipulated `init-ack` responses.
- A developer test environment accidentally connects to a malicious endpoint.

Steps:
1. Client disconnects and enters the reconnect loop.
2. The attacker's server (or a transparent proxy) responds to `culpeo.init` with a
   `culpeo.init-ack` containing `"resume_offset": 0` for an active input stream.
3. Client sets `SendOffset = 0`, believing the server wants data replayed from the start.
4. The real server (once connectivity is restored) receives duplicate frames from offset 0
   to the previous high-water mark, causing audio/data duplication or confusion.

Alternatively, with `"resume_offset": 9223372036854775807` (`Int64.MaxValue`):
5. The first subsequent `SendMediaAsync` call computes:
   ```csharp
   stream.SendOffset = Int64.MaxValue + (positive increment)  // overflow
   ```
   producing a negative `Offset` on the media frame, which the server SHOULD reject
   as a protocol error — effectively killing the stream.

### Impact

- **Data integrity**: session state can be silently corrupted by a server that returns
  crafted offsets, causing data replay, skipping, or stream termination.
- **Denial of service**: an overflow-induced negative offset causes immediate stream
  failure after resumption.
- The attack requires either `AllowInsecureConnections = true` or a compromised server,
  but the client provides no defence-in-depth to detect the manipulation.

### Proposed Mitigation

1. **Validate `resume_offset` is non-negative and does not exceed the client's tracked
   offset**:
   ```csharp
   var confirmedOffset = roProp.GetInt64();
   if (confirmedOffset < 0)
       throw new InvalidOperationException(
           "culpeo.init-ack resume_offset must not be negative.");
   if (streamType is CulpeoStreamType.Input or CulpeoStreamType.Duplex)
   {
       // Server MUST NOT claim it received more than we sent
       if (confirmedOffset > (existing?.SendOffset ?? 0))
           throw new InvalidOperationException(
               "culpeo.init-ack resume_offset exceeds client send offset.");
       state.SendOffset = confirmedOffset;
   }
   ```

2. **Log a warning** (without token content) when the server's confirmed offset differs
   from the client's expected offset by more than a configurable threshold.

3. **Document the trust boundary**: add an XML doc note that `resume_offset` is only as
   trustworthy as the server and the transport layer.

### Spec Reference

CulpeoStream spec §8.2 (session resumption), §7.2 (resume_offset in init-ack).
