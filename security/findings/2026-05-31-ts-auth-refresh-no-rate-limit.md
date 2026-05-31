## Finding: SEC-021 — requestAuthRefresh() Exposed on IServerSession With No Per-Session Rate Limit
**Severity:** Low
**Target:** TypeScript
**Phase:** Phase 3 — `culpeostream-server`

### Description

`IServerSession.requestAuthRefresh()` is part of the public handler API exposed to
application code:

```typescript
// handler.ts
export interface IServerSession {
    requestAuthRefresh(): Promise<void>;
    // ...
}
```

The `ServerSessionImpl` implementation delegates directly to
`coreSession.requestAuthRefresh()` with no rate limiting, back-pressure, or minimum
interval enforcement:

```typescript
// server.ts
public async requestAuthRefresh(): Promise<void> {
    await this.coreSession.requestAuthRefresh();
}
```

The server's own `authRefreshTimer` is rate-limited to `authRefreshIntervalMs`
(default: disabled), but this only applies to the timer-driven path. Nothing prevents an
application handler from calling `session.requestAuthRefresh()` in a loop:

```typescript
// Buggy or malicious handler
async onMedia(session, streamId, data, offset) {
    await session.requestAuthRefresh(); // called on every media frame
}
```

Each call causes the server to send a `culpeo.auth-refresh` challenge to the client.
The C# client's `HandleAuthRefreshAsync` calls the `GetToken` callback on each challenge;
if `GetToken` calls an external IdP, each call involves an outbound HTTP request.

By contrast, the C++ core session enforces `min_auth_refresh_interval_s` (typically 30 s)
at the session layer. The TypeScript server bypasses this protection when
`requestAuthRefresh()` is called directly from a handler.

### Attack Scenario

1. An application handler contains a programming error (or deliberate abuse) that calls
   `session.requestAuthRefresh()` on every incoming media frame.
2. A client streaming at 50 media frames per second receives 50 auth-refresh challenges
   per second.
3. Each challenge triggers a call to `GetToken`, which calls an external token endpoint.
4. The token endpoint is flooded with refresh requests from a single client session,
   consuming quota and potentially triggering rate-limit responses that cause the client
   to close with `auth-expired`.

### Impact

- **Client-side DoS**: excessive `GetToken` calls can exhaust client-side token endpoint
  quota, causing auth failures and session termination.
- **Third-party amplification**: if the IdP rate-limits per IP and the client and server
  are behind the same IP, a single buggy handler can exhaust the shared quota for all
  clients.
- Impact is bounded to one session per handler invocation path and requires a buggy or
  malicious handler, hence Low severity. It becomes Medium if the handler is
  multi-tenant or the `requestAuthRefresh()` path is reachable from untrusted input.

### Proposed Mitigation

1. **Enforce a minimum interval in `ServerSessionImpl.requestAuthRefresh()`**:
   ```typescript
   private lastAuthRefreshAt = 0;
   private readonly minAuthRefreshIntervalMs = 30_000; // configurable

   public async requestAuthRefresh(): Promise<void> {
       const now = Date.now();
       if (now - this.lastAuthRefreshAt < this.minAuthRefreshIntervalMs) {
           return; // silently drop — don't throw, don't log token info
       }
       this.lastAuthRefreshAt = now;
       await this.coreSession.requestAuthRefresh();
   }
   ```

2. **Expose `minAuthRefreshIntervalMs` as a `CulpeoServerOptions` field** so operators
   can tune it. The default should match the C++ core default (30 s).

3. **Document** in `IServerSession` that `requestAuthRefresh()` is subject to a minimum
   interval and that redundant calls are silently dropped.

### Spec Reference

CulpeoStream spec §6.1 (`culpeo.auth-refresh` — server-initiated token challenge).
Existing finding SEC-012 covers the C# server-side equivalent.
