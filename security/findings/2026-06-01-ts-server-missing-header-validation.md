# [SEC-030] TypeScript server: missing `Content-Type` and `Culpeostream-Version` header validation

**Severity:** Medium  
**Component:** TypeScript  
**Phase:** 4  
**Status:** Open  

## Description

`CulpeoHttp2Server.listen()` dispatches every HTTP/2 POST request to the
application handler after only checking the `:method` pseudo-header and
optionally the `:path`:

```typescript
const onStream = (stream: ServerHttp2Stream, headers: IncomingHttpHeaders) => {
  const method = headers[":method"];
  const reqPath = headers[":path"];

  if (method !== "POST") {
    stream.respond({ ":status": 405 });
    stream.end();
    return;
  }

  if (typeof reqPath === "string" && reqPath !== path && path !== "/") {
    stream.respond({ ":status": 404 });
    stream.end();
    return;
  }

  // Immediately responds 200 and dispatches the handler — no further checks.
  stream.respond({ ":status": 200, "content-type": "application/culpeostream" });
  // …
};
```

Neither `content-type: application/culpeostream` nor `culpeostream-version`
(required by Addendum C.2) is validated.  Any HTTP/2 POST to the server,
regardless of its declared content type or protocol version, receives a 200 OK
and the application handler is invoked.

By contrast, the **C# server** (`CulpeoHttp2Server.cs`) validates both headers
and returns 400 if either is missing or incorrect.

## Impact

1. **Protocol confusion:** A non-CulpeoStream HTTP/2 client (e.g., a REST API
   client, a health-check probe) that happens to POST to the same path will
   have a handler invoked against it.  The handler will wait for a
   CulpeoStream frame that never arrives, tying up handler resources until a
   timeout or disconnect.

2. **Version negotiation bypass:** A client sending `Culpeostream-Version: 2.0`
   is silently accepted.  If the server implements breaking changes in a future
   version, the current server will process frames from a newer client without
   any negotiation, potentially misinterpreting frame structure.

3. **Reduced defense-in-depth at protocol boundary:** Removing this validation
   widens the attack surface by accepting traffic that should be rejected at the
   earliest possible point.

## Location

`implementations/typescript/packages/culpeostream-http2/src/server.ts`,
`CulpeoHttp2Server.listen()`, the `onStream` callback (approximately line 120).

## Recommendation

Add the two validation checks before dispatching to the handler:

```typescript
const contentType = headers["content-type"];
if (contentType !== "application/culpeostream") {
  stream.respond({ ":status": 415 }); // Unsupported Media Type
  stream.end("Expected Content-Type: application/culpeostream");
  return;
}

const version = headers["culpeostream-version"];
if (typeof version !== "string") {
  stream.respond({ ":status": 400 });
  stream.end("Missing required Culpeostream-Version header.");
  return;
}
// Optionally validate the version value:
// if (version !== "1.0") { stream.respond({ ":status": 400 }); … }
```

This aligns the TypeScript server with the C# server's validation behaviour and
satisfies Addendum C.2 of the spec.
