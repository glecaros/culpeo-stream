## Finding: SEC-018 — Static Authorization Token Reused on All Reconnects; GetToken Never Called Proactively
**Severity:** Medium
**Target:** C#
**Phase:** Phase 3 — `CulpeoStream.Client`

### Description

`CulpeoStreamClient.PerformHandshakeAsync()` always sends `_options.Authorization` — the
static bearer token supplied at construction time — as the `Authorization` header of
every `culpeo.init` frame, including those sent during the reconnection loop
(`ReconnectLoopAsync`):

```csharp
// CulpeoStreamClient.cs, PerformHandshakeAsync()
var initFrame = new CulpeoMessage(
    CulpeoMessageKind.Control,
    Encoding.UTF8.GetBytes(initBody),
    @event: "culpeo.init",
    authorization: _options.Authorization,   // ← always the initial static token
    ...);
```

`CulpeoStreamClientOptions.GetToken` is documented as the callback for obtaining a fresh
token and is invoked **only** when the server issues a `culpeo.auth-refresh` challenge
during an established session (see `HandleAuthRefreshAsync`). It is never called
proactively to obtain a fresh credential for a new `culpeo.init` on reconnect.

Consequences:

1. **Token expiry during disconnect causes a reconnect storm.** If the bearer token
   expires while the client is disconnected, every reconnect attempt will fail with
   `auth-expired`. The reconnect loop exhausts all `MaxReconnectAttempts` (default: 10)
   without ever calling `GetToken`. The client settles into the `Disconnected` state
   silently, with no indication that a fresh token was available.

2. **GetToken semantics are misleading.** A developer who supplies a `GetToken` callback
   (which returns short-lived tokens from an IdP) reasonably expects it to be used for
   reconnects. The current design silently ignores `GetToken` for the most important
   authentication event — re-establishing the session.

3. **Long-lived sensitive credential kept in memory.** The longer a token must remain
   valid to survive reconnects, the longer the static secret lives in the
   `CulpeoStreamClientOptions` object on the heap, increasing the exposure window in
   memory-dump scenarios.

### Attack Scenario

1. Client is constructed with `Authorization = "Bearer eyJ.short-lived.token"` and
   `GetToken` callback that fetches fresh tokens from an IdP.
2. Connection drops after 55 minutes; the token has a 60-minute lifetime.
3. Before the reconnect succeeds, the token expires (e.g., reconnect is delayed by
   backoff, server downtime, or network issues that push the gap past 60 minutes).
4. Every `culpeo.init` sent during the reconnect loop receives `init-error [auth-expired]`
   from the server.
5. After 10 failed attempts the client emits `Disconnected("Max reconnection attempts
   reached.")` — even though `GetToken` could have returned a valid credential.

### Impact

- **Availability**: sessions with short-lived tokens cannot auto-recover from disconnects
  that span a token expiry window, despite the client appearing to support this via
  `GetToken`.
- **Security hygiene**: the static token is unnecessarily long-lived in memory compared
  to a design where `GetToken` is called on every (re)connect.

### Proposed Mitigation

1. **Call `GetToken` before every `culpeo.init`** (including the first connect):
   ```csharp
   private async Task<string> GetCurrentTokenAsync(CancellationToken ct)
   {
       if (_options.GetToken is not null)
           return await _options.GetToken(ct).ConfigureAwait(false);
       return _options.Authorization;
   }
   ```
   Then replace `authorization: _options.Authorization` with
   `authorization: await GetCurrentTokenAsync(cancellationToken)` in
   `PerformHandshakeAsync`.

2. Update the `CulpeoStreamClientOptions` XML doc to clarify the callback is used for
   both proactive reconnect auth and reactive `auth-refresh` responses.

3. Consider making `Authorization` optional (`string?`) when `GetToken` is provided, to
   avoid holding a stale credential in memory.

### Spec Reference

CulpeoStream spec §3.1 (bearer token in `culpeo.init`), §6.1 (`culpeo.auth-refresh`).
