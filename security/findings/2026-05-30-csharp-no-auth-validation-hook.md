# SEC-008: No Auth Validation Hook — Any Non-Empty Token Establishes a Session (Critical)

**Date:** 2026-05-30  
**Severity:** Critical  
**Component:** C# — `CulpeoStream.AspNetCore` / `ICulpeoStreamHandler`

## Description

`CulpeoConnection.HandleInitialFrame` only checks that the `Authorization` header is
non-empty. It never validates the token value and `ICulpeoStreamHandler` receives no
opportunity to reject the connection before `culpeo.init-ack` is sent and a session is
allocated. A token value of `"x"` is treated identically to a valid JWT.

## Attack Scenario

1. Attacker connects and sends `culpeo.init` with `Authorization: Bearer GARBAGE`.
2. Server sends `culpeo.init-ack`, allocates a `SessionSnapshot`, saves it to the session
   store, and calls `OnConnectedAsync`.
3. Attacker now has a fully established session with a valid server-assigned session ID,
   confirmed streams, and a full buffer window — even if the handler calls `CloseAsync`.

Contrast with C++, which invokes `on_auth_validate` **before** sending `culpeo.init-ack`.

## Recommendation

Add `Task<bool> AuthenticateAsync(string authorization)` to `ICulpeoStreamHandler` and
invoke it before sending `culpeo.init-ack`. Only send the ack if it returns true.

## Status

Open — **Blocker for any production deployment**
