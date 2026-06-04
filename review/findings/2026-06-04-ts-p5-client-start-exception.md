# Client `connect()` Promise hangs permanently if `session.start()` throws

**Severity:** Medium

**Location:** `implementations/typescript/packages/culpeostream-client/src/client.ts`, lines 400–435

## Description

`runConnection` is launched with `void this.runConnection()` (line 347), meaning the caller never awaits it and any unhandled rejection is silently discarded. `session.start()` is called without a surrounding `try/catch`:

```typescript
// Build and start session
const session = new CulpeoClientSession({ ... });
this.session = session;

await session.start({           // ← if this throws, the function exits here
  authorization,
  bufferWindowMs: options.bufferWindowMs ?? 5000,
  resumeFrom,
});

// Receive loop
try {
  for await (const msg of wsClient) { ... }
} catch { /* ... */ }

// ... scheduleReconnect() is only reached if session.start() returned normally
```

If `session.start()` throws — for example because `options.version` is invalid, the core session is in an unexpected state on resumption, or the outbound `sendFrame` / `wsClient.send` path raises a synchronous error — execution exits `runConnection` immediately. The `pendingConnect` promise (set by `connect()`) is never resolved or rejected. `connect()` then hangs forever with no timeout and no error.

The `connect()` call-site pattern that will deadlock:

```typescript
const client = new CulpeoStreamClient();
await client.connect(url, options);  // never settles if session.start() throws
```

Unlike the WebSocket-connect failure path (lines 362–367), which correctly calls `scheduleReconnect()`, the `session.start()` failure path has no recovery.

## Impact

Any consumer `await`-ing `client.connect(...)` will be permanently blocked. In server-side Node.js applications this ties up an async context indefinitely. The root cause of the failure is invisible: no `"error"` event is emitted, no rejection propagates, no timeout fires.

## Suggested Fix

Wrap `session.start()` (and the rest of `runConnection` below it) in a `try/catch` that either schedules a reconnect or rejects the pending connect promise:

```typescript
try {
  await session.start({ authorization, bufferWindowMs: options.bufferWindowMs ?? 5000, resumeFrom });
} catch (err) {
  const startErr = err instanceof Error ? err : new Error(String(err));
  // Reject the pending connect promise if still outstanding.
  const pending = this.pendingConnect;
  this.pendingConnect = undefined;
  if (pending !== undefined) {
    pending.reject(startErr);
  } else {
    this.emit("error", startErr);
  }
  return; // do not attempt reconnect; this is a configuration/state error
}
```

Alternatively, consider making the entire body of `runConnection` (after the WebSocket connect) a single `try/catch` that funnels all unexpected errors into `scheduleReconnect` or `pendingConnect.reject`.
