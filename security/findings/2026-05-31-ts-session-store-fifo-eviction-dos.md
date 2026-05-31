## Finding: SEC-022 — InMemorySessionStore FIFO Eviction Enables Authenticated Session-Eviction DoS
**Severity:** Medium
**Target:** TypeScript
**Phase:** Phase 3 — `culpeostream-server`

### Description

`InMemorySessionStore.save()` implements a FIFO (first-in, first-out) eviction strategy
using JavaScript `Map`'s guaranteed insertion-order iteration:

```typescript
// store.ts
if (
    !this.sessions.has(snapshot.sessionId) &&
    this.sessions.size >= this.maxSessions
) {
    // Evict the oldest entry (Map preserves insertion order).
    const oldest = this.sessions.keys().next().value;
    if (oldest !== undefined) {
        this.sessions.delete(oldest);
    }
}
```

The `maxSessions` default is **1,000**. An authenticated attacker who can open new
WebSocket connections can fill the store to capacity and then continuously open new
connections at a rate that outpaces legitimate session establishment, forcing the eviction
of all legitimate sessions.

Evicted sessions cannot be resumed: when a reconnecting client sends `culpeo.init` with
its `Session-Id`, `store.load()` returns `null` and the server treats it as a fresh
session. The client's buffered resumption state (stream offsets, queued frames) is lost.

There is no separate per-user quota, no per-IP quota, and no distinction between active
and inactive entries during eviction.

### Attack Scenario

1. The service authenticates any registered user. The attacker holds a valid token.
2. The attacker opens 1,000 WebSocket connections in rapid succession, each completing
   the `culpeo.init` handshake. The store is now full of attacker sessions.
3. The attacker continues opening new connections (any single connection can be
   immediately dropped after the snapshot is saved).
4. Each new attacker session evicts the oldest entry — which is now a legitimate user's
   session snapshot.
5. When legitimate users disconnect and attempt to resume, `store.load()` returns `null`.
   The server creates a fresh session; the client receives `SessionResumed` but with
   zeroed stream offsets, losing any buffered data and continuity.
6. By maintaining the flood at a rate ≥ (legitimate reconnect rate), the attacker can
   permanently prevent any legitimate session from surviving a disconnect.

The cost to the attacker per eviction is one authenticated WebSocket handshake (two
frames: `culpeo.init` + `culpeo.init-ack`). At 1,000 connections per second (achievable
on a well-connected machine), the entire store turns over every second.

### Impact

- **Session availability DoS**: legitimate sessions cannot resume after any disconnect
  while the attack is ongoing, causing data-stream interruption and state loss.
- **Amplification**: the attacker's cost is one handshake per victim eviction; the victim
  loses an entire session's state and must re-establish from scratch.
- Requires authentication, which limits the attacker population but does not prevent abuse
  from compromised accounts, misconfigured clients, or low-cost account registration.

### Proposed Mitigation

1. **Per-authenticated-identity session quota**: track how many sessions are stored per
   identity (e.g., derived from the token's `sub` claim stored in the snapshot). Reject
   `save()` if the quota is exceeded rather than evicting a different user's entry.

2. **Eviction preference**: when capacity is reached, prefer evicting expired entries
   first, then the attacker's own oldest entries (if per-user tracking is available),
   before resorting to global FIFO.

3. **Minimum: add an eviction warning log** that alerts operators when eviction is
   occurring at an anomalous rate, enabling detection and investigation.

4. **Capacity tuning guidance**: document `maxSessions` in the context of expected
   concurrent clients, recommending that operators set it well above their P99 concurrent
   session count plus a safety margin.

5. **Rate-limit connections per IP or per identity** at the HTTP layer before sessions
   reach the store — this is the most effective defence-in-depth layer and is outside the
   scope of `CulpeoServer` itself, but should be noted in operational guidance.

### Spec Reference

CulpeoStream spec §8.2 (session resumption, buffer-window). The spec does not mandate a
specific store eviction strategy; this is an implementation-quality finding.
