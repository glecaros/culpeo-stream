# SEC-013: Documentation Claims RAND_bytes/getrandom — Implementation Uses std::random_device

**Date:** 2026-05-30  
**Severity:** Medium  
**Component:** C++ — `libculpeo-session/src/crypto.hpp`

## Description

Comments and documentation claim nonces use `RAND_bytes` / `getrandom`. The actual
implementation uses `thread_local std::random_device` in a fill loop. While this is
CSPRNG-backed on major platforms, it differs meaningfully: `std::random_device` can
fail silently on entropy-constrained environments, each thread opens a new fd to
`/dev/urandom`, and `/dev/urandom` (not `getrandom`) does not block until entropy is
available. Documentation misleads security reviewers.

## Recommendation

Replace `thread_local std::random_device` with:
- Linux: `getrandom(buf, len, 0)`
- macOS: `arc4random_buf(buf, len)`
- Windows: `BCryptGenRandom`

Update all documentation to reflect the actual primitive used.

## Status

Open
