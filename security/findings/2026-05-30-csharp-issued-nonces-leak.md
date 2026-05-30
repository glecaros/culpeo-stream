# SEC-015: issuedNonces HashSet Accumulates Unconsumed Entries — Slow Memory Leak

**Date:** 2026-05-30  
**Severity:** Low  
**Component:** C# — `CulpeoConnection`

## Description

Every `IssueAuthRefreshAsync` call adds a nonce to `issuedNonces`. Only a successfully
validated response removes it. Over long-lived sessions with repeated re-auth or dropped
responses, the set grows without bound. The HashSet provides no security benefit beyond
the `pendingNonce` field.

## Recommendation

Remove `issuedNonces` entirely. Validate solely against `pendingNonce` using
`CryptographicOperations.FixedTimeEquals`. The 30-second timeout already constrains
the replay window.

## Status

Open
