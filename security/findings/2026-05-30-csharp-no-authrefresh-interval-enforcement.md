# SEC-012: No Minimum Interval on Auth-Refresh Issuance — Client Flood Vector + Nonce Leak

**Date:** 2026-05-30  
**Severity:** Medium  
**Component:** C# — `CulpeoConnection.IssueAuthRefreshAsync`

## Description

`IssueAuthRefreshAsync` issues a new nonce unconditionally with no re-issue interval.
Each call overwrites `pendingNonce` but leaves the old nonce in `issuedNonces`. A
component calling this in a loop floods the client with `culpeo.auth-refresh` frames,
each requiring an async `getToken()` call. The `issuedNonces` HashSet also grows without
bound (see SEC-015).

The C++ implementation enforces `min_auth_refresh_interval_s` (default 30 s).

## Recommendation

1. Enforce a minimum re-issue interval (configurable, default 30 s), matching C++.
2. Reject if a nonce is already pending.
3. Remove `issuedNonces` HashSet (see SEC-015).

## Status

Open
