# [SEC-026] TypeScript client: oversized-frame `RangeError` in `frames()` data handler is uncaught — process crash

**Severity:** High  
**Component:** TypeScript  
**Phase:** 4  
**Status:** Open  

## Description

In `client.ts`, the `Http2ConnectionImpl.frames()` async iterator attaches a
`stream.on("data", …)` listener that calls the inner `flush()` function without
a `try/catch`:

```typescript
stream.on("data", (chunk: Buffer | string) => {
  const data = Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk as string);
  buf = Buffer.concat([buf, data]);
  flush();                    // ← RangeError propagates out of here
  if (resolve !== null && queue.length > 0) { … }
});
```

`flush()` calls `decodeFrame(buf)` which throws `RangeError` when the
encoded payload length exceeds `maxPayloadBytes`.  Because the throw
escapes the synchronous event-listener callback, Node.js treats it as an
**unhandled exception on the event loop**.  Unless the process has registered
an `uncaughtException` handler, Node.js terminates the process.

By contrast, the **server-side** implementation in `server.ts` already wraps
`flush()` correctly:

```typescript
try {
  flush();
} catch (err: unknown) {
  error = err;
  done = true;
  if (reject !== null) { … rej(err); }
  return;
}
```

The client was not updated to match.

## Impact

A malicious (or buggy) server can crash the client process by sending a single
frame with a payload length field larger than `maxPayloadBytes` (default
16 MiB).  No authentication is required — the frame is sent before any
application-level auth check.  Any Node.js client using this library against an
untrusted or compromised server is vulnerable to remote process termination.

## Location

`implementations/typescript/packages/culpeostream-http2/src/client.ts`,
inside `Http2ConnectionImpl.frames()`, the `stream.on("data", …)` callback
(approximately line 95).

## Recommendation

Apply the same try/catch pattern the server already uses:

```typescript
stream.on("data", (chunk: Buffer | string) => {
  const data = Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk as string);
  buf = Buffer.concat([buf, data]);
  try {
    flush();
  } catch (err: unknown) {
    done = true;
    if (reject !== null) {
      const rej = reject;
      resolve = null;
      reject = null;
      rej(err);
    }
    return;
  }
  if (resolve !== null && queue.length > 0) {
    const item = queue.shift()!;
    const res = resolve;
    resolve = null;
    reject = null;
    res({ value: item, done: false });
  }
});
```

This routes the error through the iterator's rejection path instead of letting
it escape to the event loop.
