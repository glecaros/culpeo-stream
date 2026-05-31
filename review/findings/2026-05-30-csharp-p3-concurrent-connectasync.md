# CS-P3-001: Race in ConnectAsync — Check-Then-Set on _loopTask Not Synchronized

**Date:** 2026-05-30  
**Severity:** High  
**Component:** C# — `CulpeoStreamClient.ConnectAsync`

## Description

`ConnectAsync` checks `if (_loopTask is not null)` then assigns it, without any
synchronization. Two concurrent callers can both pass the null check and both start
a reconnect loop.

## Recommendation

Guard `ConnectAsync` with a `SemaphoreSlim(1,1)` or use
`Interlocked.CompareExchange` on `_loopTask`.

## Status

Open
