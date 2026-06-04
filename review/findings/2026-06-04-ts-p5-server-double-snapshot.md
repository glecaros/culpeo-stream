# Server saves session snapshot twice on client-initiated `culpeo.close`

**Severity:** Low

**Location:** `implementations/typescript/packages/culpeostream-server/src/server.ts`, lines 527–531 and lines 599–611

## Description

When a client sends `culpeo.close`, the notification handler explicitly saves the snapshot and then closes the WebSocket:

```typescript
// handleNotification — "close" case (line 527)
case "close": {
  await this.saveSnapshot();                                      // (1) first save
  await this.wsClient.close(1000, n.frame.headers.reason);
  break;
}
```

Closing the WebSocket causes the `for await...of` loop in `runMessageLoop` to exit, which unconditionally calls `handleWsClose`:

```typescript
// runMessageLoop (line 363)
await this.handleWsClose("WebSocket closed");
```

`handleWsClose` saves the snapshot a second time:

```typescript
// handleWsClose (line 546)
private async handleWsClose(reason: string): Promise<void> {
  if (this.closed) return;
  this.closed = true;
  this.stopTimers();
  await this.saveSnapshot();      // (2) second save — duplicate
  ...
}
```

The `this.closed` guard prevents `handleWsClose` from running *its body* twice, but it does not prevent the double save that arises from save (1) + save (2) across the two code paths. Both saves occur with high probability on every graceful client-initiated close.

## Impact

For the default `InMemorySessionStore` this is benign — the second write simply overwrites with identical data. For pluggable external stores (databases, Redis, etc.) the duplicate write has several potential consequences:

- **Extra latency / throughput cost**: two round-trips to the store per close.
- **Write-ordering hazards with non-idempotent stores**: if a store uses optimistic locking or conditional writes, the second write may fail or inadvertently overwrite a concurrent update from a different process that picked up the snapshot between the two saves.
- **Audit log noise**: stores that track write history will record a spurious duplicate entry.

## Suggested Fix

Remove the explicit `saveSnapshot()` call from the `"close"` notification handler and rely on the existing `handleWsClose` call to perform the single authoritative save after the loop exits:

```typescript
case "close": {
  // saveSnapshot() is called by handleWsClose() after the loop exits.
  await this.wsClient.close(1000, n.frame.headers.reason);
  break;
}
```

The `wsClient.close(1000, ...)` will cause the `for await` loop to terminate, which then calls `handleWsClose`, which saves the snapshot. One save, one path.

If there is a concern that the snapshot must be saved *before* the WebSocket close acknowledgement is sent to the client, document that requirement explicitly and remove the duplicate in `handleWsClose` instead.
