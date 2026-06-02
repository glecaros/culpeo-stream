# [SEC-027] TypeScript client: `rejectUnauthorized: false` silently disables TLS verification with no warning

**Severity:** Medium  
**Component:** TypeScript  
**Phase:** 4  
**Status:** Open  

## Description

`CulpeoHttp2Client` exposes `rejectUnauthorized` as an option:

```typescript
export interface CulpeoHttp2ClientOptions {
  rejectUnauthorized?: boolean; // default: true
  // ...
}
```

When set to `false`, the underlying `http2.connect(authority, { rejectUnauthorized })` call accepts any TLS certificate — including self-signed or expired ones — without any visible indication that security has been downgraded.

The **server** (`CulpeoHttp2Server`) applies the analogous `allowInsecure` option and correctly emits a prominent runtime warning:

```typescript
if (options.allowInsecure === true) {
  console.warn(
    "[culpeostream-http2] CulpeoHttp2Server: allowInsecure is enabled. " +
    "This uses plaintext HTTP/2 (h2c) and must NOT be used in production.",
  );
}
```

The **client** emits no corresponding warning when `rejectUnauthorized: false`
is supplied.  A developer who copies a test configuration into production
silently ships an MITM-vulnerable client.

## Impact

TLS peer authentication is disabled without any indication in runtime logs or
console output.  An on-path attacker presenting any certificate (e.g. a
self-signed cert for the same hostname) can terminate the TLS session and read
or inject all frames — including bearer tokens in `culpeo.init`.  This is a
MITM vector equivalent to the `verify_none` bug filed in SEC-023 for the C++
client.

## Location

`implementations/typescript/packages/culpeostream-http2/src/client.ts`,
`CulpeoHttp2Client` constructor / `connect()` method (options object and
`http2.connect` call).

## Recommendation

Emit a `console.warn` (matching the server-side pattern) when
`rejectUnauthorized` is explicitly `false`:

```typescript
constructor(options: CulpeoHttp2ClientOptions) {
  if (options.rejectUnauthorized === false) {
    console.warn(
      "[culpeostream-http2] CulpeoHttp2Client: rejectUnauthorized is false. " +
      "TLS certificate validation is disabled. " +
      "This MUST NOT be used in production.",
    );
  }
  // …
}
```

This mirrors the existing `allowInsecure` warning in `CulpeoHttp2Server` and
gives operators visibility in logs when the setting is active.
