# TS-P3-003: InMemorySessionStore Evicts by Insertion Order — Active Sessions at Risk

**Date:** 2026-05-30  
**Severity:** Medium  
**Component:** TypeScript — `culpeostream-server/src/store.ts` `InMemorySessionStore`

## Description

When `maxSessions` is reached, eviction is based on Map insertion order (oldest first).
A long-running session that never reconnects sits at the front and gets evicted first,
even if it is actively processing frames. If its snapshot is evicted while connected,
the session cannot resume after a network drop.

## Recommendation

Track last-access time and evict least-recently-used. Touch the entry in `load()` and
`save()` to keep active sessions at the end of the eviction queue.

## Status

Open
