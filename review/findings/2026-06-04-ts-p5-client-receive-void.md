# Client `session.receive` errors silently dropped — no `error` event emitted

**Severity:** High

**Location:** `implementations/typescript/packages/culpeostream-client/src/client.ts`, lines 408–419

## Description

Inside `runConnection`'s receive loop, `session.receive(frame)` is called with `void`:

```typescript
for await (const msg of wsClient) {
  try {
    const frame = msg.binary
      ? parseFrame(new Uint8Array(msg.data as ArrayBuffer), "binary")
      : parseFrame(msg.data as string, "text");
    void session.receive(frame);      // ← fire-and-forget; async errors lost
  } catch (err) {
    this.emit("error", err instanceof Error ? err : new Error(String(err)));
  }
}
```

The `try/catch` only intercepts *synchronous* throws from `parseFrame`. Because `session.receive` is an `async` function, any rejection it produces is discarded silently — it does not reach the `catch` block and is not emitted as an `"error"` event. JavaScript runtime will report it as an unhandled promise rejection (a global event, not a per-instance one), which in Node.js causes a process-level warning and, in newer versions, a crash if `--unhandled-rejections=throw` is set.

The server implementation (`server.ts` line 350) correctly `await`s the equivalent call:
```typescript
await this.coreSession.receive(frame);
```
and wraps it in a `try/catch` that terminates the connection on error. The client does neither.

### Concrete failure modes

1. **Auth-refresh failure**: `session.receive` processes `culpeo.auth-refresh`, invokes the user-supplied `getToken()` callback, and awaits it. If `getToken()` rejects, the rejection propagates out of `session.receive`. The rejection is dropped; `culpeo.auth-response` is never sent; the server eventually terminates the session — but the client never emits `"error"` and has no idea why.

2. **State machine violation**: A server bug or interop partner sends a frame in the wrong session state. `session.receive` may throw a `CulpeoError("protocol-error", ...)`. That error is silently swallowed; the client continues running as if nothing happened.

3. **Unhandled rejection noise**: In Node.js with `--unhandled-rejections=throw` (the default since Node 15), the first such rejection crashes the process.

## Impact

Protocol errors and token-refresh failures in the client are invisible to application code. The client can appear to operate normally while in a broken state. Security-relevant failures (expired token not refreshed, auth challenge not answered) are lost without any observable signal.

## Suggested Fix

`await` `session.receive` inside the existing `try/catch`, matching the server pattern:

```typescript
for await (const msg of wsClient) {
  try {
    const frame = msg.binary
      ? parseFrame(new Uint8Array(msg.data as ArrayBuffer), "binary")
      : parseFrame(msg.data as string, "text");
    await session.receive(frame);    // ← was: void session.receive(frame)
  } catch (err) {
    this.emit("error", err instanceof Error ? err : new Error(String(err)));
  }
}
```

This ensures every error from `session.receive` — including async rejections — is reported via the `"error"` event and does not become an unhandled rejection.
