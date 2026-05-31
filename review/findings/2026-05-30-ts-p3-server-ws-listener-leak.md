# TS-P3-001: Server WebSocket Event Listeners Never Removed — Memory Leak

**Date:** 2026-05-30  
**Severity:** Medium  
**Component:** TypeScript — `culpeostream-server/src/server.ts` `ServerConnection`

## Description

`ws.on('message'/'close'/'error')` listeners registered in the `ServerConnection`
constructor are never removed. When the connection closes, the WebSocket's internal
listener arrays retain references to the closures (and the session state they close
over), preventing garbage collection.

## Recommendation

In `handleWsClose()`, call:
```typescript
this.ws.removeAllListeners('message');
this.ws.removeAllListeners('close');
this.ws.removeAllListeners('error');
```

## Status

Open
