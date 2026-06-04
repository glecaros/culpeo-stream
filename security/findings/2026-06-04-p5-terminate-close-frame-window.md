## Finding: Server — graceful terminate() creates a window for post-close frame processing

**Severity:** Low
**Target:** TypeScript
**Phase:** Phase 5 — @culpeo/async-ws adoption / server.ts

### Description

The Phase 5 change replaces the hard TCP RST in `terminate()` with a graceful
WebSocket Close frame:

```typescript
/** Close the connection immediately (for server shutdown). */
public terminate(): void {
  void this.wsClient.close(1000, "Server shutdown");
}
```

`wsClient.close(1000, ...)` from `@culpeo/async-ws` sends a WebSocket Close
frame and then waits for the peer's echoed Close frame before tearing down the
socket.  During this handshake interval, the async message loop
(`runMessageLoop`) continues to iterate via `for await (const msg of
this.wsClient)`.

Depending on the implementation of `WebSocketClient` in `@culpeo/async-ws`, the
iterator may or may not continue to yield messages that arrive after `close()` is
called but before the peer acknowledges the Close.  If it does yield those
messages, frames sent by a client immediately after receiving the server's Close
frame will still be parsed, authenticated (since `this.coreSession` is still set
and `this.initialized` is still `true`), and dispatched to the application
handler.

The scenario is relevant specifically during `CulpeoServer.close()`:

```typescript
for (const conn of this.connections) {
  conn.terminate();      // sends Close, does NOT await
}
this.connections.clear();  // connection removed from tracking immediately
```

Because `terminate()` is `void` (fire-and-forget), the connections are cleared
while their async message loops are still running.  A fast client can inject one
or more additional application-level frames between the server's Close frame and
the eventual socket teardown.

### Attack Scenario

1. Server calls `CulpeoServer.close()` during shutdown.
2. Server sends Close frame (1000) to an established client.
3. Before the client echoes the Close, it sends one or more media or event
   frames.
4. The server's `runMessageLoop` receives and dispatches these frames to the
   application handler (`onMedia`, `onEvent`).
5. The application handler processes the frames as legitimate session traffic
   even though the server has already committed to shutting down.

In a worst-case scenario, an attacker delays their Close echo indefinitely
(within the TCP keep-alive window) and continues injecting frames, effectively
holding the session alive past server shutdown intent.

### Impact

Continued session activity and application handler invocations after server
shutdown intent.  For most applications this is a low-severity race condition.
For applications that make hard security assumptions about event ordering after a
close (e.g., "no new media can arrive after shutdown begins"), this could cause
logic vulnerabilities.

### Suggested Fix

1. Set a boolean flag `this.closed = true` inside `terminate()` before calling
   `wsClient.close()`, and check it at the top of the frame-processing inner
   loop:

   ```typescript
   public terminate(): void {
     this.closed = true;           // NEW
     void this.wsClient.close(1000, "Server shutdown");
   }
   ```

   The existing `if (this.closed) return;` guard in `handleWsClose` already
   handles post-close cleanup, but the message loop inner `catch` block does not
   check `this.closed`.

2. In `runMessageLoop`, skip frame dispatch if `this.closed`:

   ```typescript
   for await (const msg of this.wsClient) {
     if (this.closed) break;       // NEW: discard post-close messages
     try { ... }
   }
   ```

3. Consider upgrading `CulpeoServer.close()` to await `conn.terminate()` with a
   timeout before clearing the connections set, ensuring orderly draining.

### Spec Reference

N/A (transport implementation detail)
