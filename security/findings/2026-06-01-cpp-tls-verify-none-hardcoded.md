# [SEC-023] C++ TLS client hardcodes `verify_none` — peer certificate never validated

**Severity:** Critical  
**Component:** C++  
**Phase:** 4  
**Status:** Open  

## Description

`CulpeoH2Client::connect()` (`h2_client.cpp`, line 602) unconditionally sets
`asio::ssl::verify_none` on every TLS connection, regardless of the
`asio::ssl::context` the caller constructed or configured:

```cpp
ssl_sock.set_verify_mode(asio::ssl::verify_none); // tests — no CA
```

The comment says "tests — no CA" but the code is in the **only** TLS path.
Every caller of `CulpeoH2Client(ioc, tls_context)` — including production
callers who set `verify_peer` on their context — will have that setting
silently overridden to `verify_none`.  The server's TLS certificate is never
validated against a trust anchor.

## Impact

Any on-path attacker (BGP hijacker, rogue ISP, compromised load balancer, ARP
spoofer on the same LAN) can present a self-signed certificate and terminate
the TLS session.  The client will complete the handshake without complaint and
proceed to exchange CulpeoStream frames — including `culpeo.init` bearer
tokens — through the attacker's session.  This fully defeats transport-layer
confidentiality and authentication.

## Location

`implementations/cpp/libculpeo-transport-h2/src/h2_client.cpp`, line 602 (TLS
`connect()` branch, inside `CulpeoH2Client::connect()`).

## Recommendation

Remove the `set_verify_mode` call from `connect()` entirely.  The caller is
already expected to configure the `asio::ssl::context` before constructing the
client (the header doc example shows exactly this).  If a test-only override is
needed, expose it through the `AllowCleartext` pattern (i.e., a dedicated test
fixture that constructs its own permissive context) rather than silently
clobbering the caller's setting.

```cpp
// REMOVE this line from connect():
// ssl_sock.set_verify_mode(asio::ssl::verify_none); // tests — no CA

// The ssl::context passed by the caller already carries the verify mode.
// For tests without a CA, create a context with verify_none in the test
// fixture and pass that — don't override here.
```

If a default-safe fallback is needed, assert or throw if the context's verify
mode is `verify_none` when not in development mode:

```cpp
if (impl_->tls->verify_mode() == asio::ssl::verify_none) {
    // Log a warning; in strict mode, throw.
}
```
