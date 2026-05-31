## Finding: SEC-020 — Session Resumption Has No Authentication-to-Session Binding; Cross-User Session Hijacking
**Severity:** High
**Target:** TypeScript
**Phase:** Phase 3 — `culpeostream-server`

### Description

`ServerConnection.handleInitMessage()` in `server.ts` performs authentication and session
loading as two independent, unbound operations:

```typescript
// Step 1: authenticate the token (returns true/false, no session context)
authenticated = await this.options.authenticate(frame.headers.authorization);

// Step 2: load the session by ID — no check that the token owns this session
if (frame.headers.sessionId !== undefined) {
    const stored = await this.store.load(frame.headers.sessionId);
    resumeSnapshot = stored ?? undefined;
}
```

The `authenticate` callback signature is:
```typescript
authenticate: (authorization: string) => Promise<boolean>;
```

It receives only the `Authorization` header value. The `sessionId` is never passed to
`authenticate`. The `ISessionStore.load()` method is a pure key-value lookup with no
ownership semantics. There is no field in `SessionSnapshot` (as consumed here) that the
server can use to verify that the presenting token is entitled to resume the named session.

**Result**: any user whose token passes authentication can resume any other user's session
by supplying that session's ID in `Session-Id` header of `culpeo.init`.

### Attack Scenario

Prerequisites:
- Attacker has a valid bearer token (e.g., for their own account).
- Attacker has obtained Victim's session ID (possible vectors: logging leakage, shared
  infrastructure logs, exposed admin API, social engineering, or brute-force — session
  IDs from `RandomUUID()` / `crypto.randomUUID()` are 122 bits of entropy, but see below).

Steps:
1. Victim establishes session `sess-f3a9...`. Server saves snapshot to `InMemorySessionStore`.
2. Victim disconnects (e.g., temporary network loss). Snapshot is saved again in `handleWsClose`.
3. Attacker sends:
   ```
   Event: culpeo.init
   Authorization: Bearer <attacker-token>
   Session-Id: sess-f3a9...
   Content-Type: application/json

   {"version":"0.3","streams":[...]}
   ```
4. Server calls `authenticate("Bearer <attacker-token>")` → **true** (attacker has a valid
   token for their own account).
5. Server calls `store.load("sess-f3a9...")` → returns Victim's snapshot.
6. Server creates a new `CulpeoServerSession` with Victim's resume snapshot.
7. Attacker is now operating as Victim: receives buffered frames, inherits Victim's stream
   state, and is identified as Victim's session to the application handler.

If the legitimate `authenticate` backend is shared (e.g., "any registered user may
connect"), the attack requires only knowledge of Victim's session ID and possession of
any valid token — not Victim's specific credentials.

### Impact

- **Critical confidentiality**: attacker receives any buffered output frames queued for
  Victim (e.g., server → client audio, personalised data, partial AI responses).
- **Critical integrity**: attacker can send media frames that the server attributes to
  Victim's session, causing data corruption or fraudulent activity under Victim's identity.
- **Authentication bypass**: the authentication step becomes a necessary but insufficient
  gate — it only proves "this user is registered", not "this user owns this session".

### Note on Session ID Entropy

Session ID entropy is high if generated with `crypto.randomUUID()` (122 bits) making
brute-force impractical. However the vulnerability does not require brute-force — it only
requires the attacker to know one session ID, which is plausible via log leakage or an
exposed administrative interface. The spec designates session IDs as opaque secrets, but
defence-in-depth requires that knowledge of the session ID alone is insufficient to
hijack it.

### Proposed Mitigation

**Option A (preferred): Pass `sessionId` to `authenticate`**

Extend the authenticate callback signature:
```typescript
authenticate: (authorization: string, sessionId?: string) => Promise<boolean>;
```

Server implementations that require session-ownership validation (e.g., JWTs with a
`sid` claim, or lookup in their own session registry) can then enforce it. Backward
compatibility is preserved — existing callbacks that ignore the second argument continue
to work, accepting the risk of cross-session resume (which operators can document as a
known limitation).

**Option B: Add `ownerToken` (or token hash) to `SessionSnapshot`**

When a session snapshot is created on first `init-ack`, record a representation of the
authorizing credential (e.g., a hash of the bearer token, or the token's `sub` claim):
```typescript
interface SessionSnapshot {
    sessionId: string;
    ownerTokenHash: string;   // added field
    // ...
}
```
On resume, derive the same hash from the presenting token and compare before loading.
This requires no API change to `authenticate` but does bind snapshot storage to
credential material.

**Option C: Blind token to session in the store interface**

Add `loadIfOwned(sessionId: string, ownerToken: string): Promise<SessionSnapshot | null>`
to `ISessionStore`. The in-memory implementation hashes the token and stores it with the
snapshot; the lookup only returns a snapshot if the hash matches.

**Minimum remediation** (short-term, no API break): document the behaviour explicitly,
add a server warning log when `sessionId` is accepted but `authenticate` does not receive
the session ID, and update the spec to require server implementations to bind session
ownership to authentication identity.

### Spec Reference

CulpeoStream spec §5.2 (session resumption), §3.1 (authentication). The spec does not
currently require session-ownership binding — this is a spec-level gap as well as an
implementation gap.
