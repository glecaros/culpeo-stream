# TS-002: WebSocket Event Listeners Never Removed (Memory Leak)

**Date:** 2026-05-30  
**Severity:** High  
**Component:** TypeScript — `culpeostream-client/src/client.ts`

## Description

`openConnection()` attaches four event listeners (`open`, `message`, `close`, `error`)
to each new WebSocket instance but never removes them. On reconnection, the old
WebSocket is overwritten without cleanup, preventing garbage collection of
the old instance and its closure state.

## Location

`packages/culpeostream-client/src/client.ts` — `openConnection()`, ~lines 367–426

## Recommendation

Store listener references and call `removeEventListener` before overwriting `this.ws`,
or use `{ once: true }` for the `open` event and explicit cleanup in the close/error handlers.

## Status

Open
