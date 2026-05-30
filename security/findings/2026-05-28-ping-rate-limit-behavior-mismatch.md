## Finding: Ping rate limit silently drops frames instead of closing the connection

**Severity:** Medium
**Target:** C#, TypeScript
**Phase:** Phase 1

### Description

Both the C# and TypeScript DECISIONS.md files document the ping rate limiter as follows:

> "closes the connection with `rate-limit-exceeded` if more than 5 arrive within any 1-second window"

The actual code in both implementations **silently drops** excess pings without closing or signalling the peer.

**C# (`CulpeoSession.cs`):**
```csharp
private CulpeoProcessResult HandlePing(CulpeoMessage frame)
{
    ...
    if (!TryTrackPing(now))
    {
        return CulpeoProcessResult.Empty(State);  // ShouldClose = false, CloseCode = null
    }
    ...
}
```

**TypeScript (`session.ts`):**
```typescript
private async handleIncomingPing(frame: PingFrame): Promise<void> {
    ...
    if (this.receivedPingTimestampsMs.length >= 5) {
        return;   // Just returns; no close, no error
    }
    ...
}
```

Neither implementation closes the connection or emits a `culpeo.close` frame with code `rate-limit-exceeded`. The documented behavior is not what ships.

### Attack Scenario

**Primary: Undocumented divergence from spec intent**

The spec §6.1 states that a receiver MAY enforce a ping rate limit. The DECISIONS.md for both implementations specified closing with `rate-limit-exceeded` as the enforcement action. Any code that instruments `CloseCode` on a `CulpeoProcessResult` (C#) or catches a `CulpeoError` with `rate-limit-exceeded` (TypeScript) to log abuse or block repeat offenders will never receive this signal even under active ping flooding.

**Secondary: Indefinite resource lock**

An attacker who discovers the silent-drop behavior can send pings at 6/sec indefinitely. The server never closes the connection. Across many sessions, this maintains open connections while the server continues processing the first 5 pings per second on each, consuming both connection state and (minor) processing cost.

**Note on intent gap**: The silent-drop behavior is arguably *more* resilient against reconnection storms than closing would be—closing prompts well-implemented clients to reconnect, compounding load. However, the divergence between the documented decision and the shipped behavior is itself a finding, since it means security monitoring code built against the stated API contract will be silently non-functional.

### Impact

- **Observable divergence** between documented security behavior and implementation: security monitoring that relies on `rate-limit-exceeded` close codes will never fire.
- **Undocumented contract**: library consumers reading DECISIONS.md believe excess pings close the connection; they do not.
- Both implementations behave **identically but both are wrong** relative to their own documented intent, which suggests the implementation was changed after the decision was written without updating the decision log.

### Proposed Mitigation

The team must make and document a deliberate choice between two defensible options:

**Option A — Close with `rate-limit-exceeded` (matches documented intent):**

C#:
```csharp
if (!TryTrackPing(now))
{
    return CloseWithCloseFrame("rate-limit-exceeded", "Ping rate limit exceeded.");
}
```

TypeScript:
```typescript
if (this.receivedPingTimestampsMs.length >= 5) {
    await this.failWithClose("rate-limit-exceeded", "Ping rate limit exceeded.");
    return;
}
```

**Option B — Silent drop (current behavior, explicitly documented):**

Update the DECISIONS.md for both C# and TypeScript to reflect the actual behavior:

> "Excess pings beyond the 5-per-second window are silently dropped without closing the connection to avoid triggering client reconnection storms. The connection remains open."

Whichever option is chosen, both implementations must behave identically and DECISIONS.md must accurately reflect the implementation.

### Spec Reference

§6.1 — Ping/Pong: "Receivers MAY enforce a rate limit on incoming ping frames; behavior on limit exceeded is implementation-defined."
