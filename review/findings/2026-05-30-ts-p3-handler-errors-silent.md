# TS-P3-002: Handler Errors Silently Swallowed — No Observability

**Date:** 2026-05-30  
**Severity:** Medium  
**Component:** TypeScript — `culpeostream-server/src/server.ts` `handleNotification()`

## Description

Exceptions from `onMedia` and `onEvent` handlers are caught and discarded silently.
The frame is lost with no indication to the client or any log output. Operators have
no way to detect application-level handler failures.

## Recommendation

At minimum emit a warning log. Consider adding an `onError` hook to
`ICulpeoStreamHandler` for handler-level errors, or expose errors via an `error` event
on `CulpeoServer`.

## Status

Open
