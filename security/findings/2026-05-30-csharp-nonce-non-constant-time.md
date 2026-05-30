# SEC-011: Auth-Response Nonce Comparison Uses Non-Constant-Time string.Equals — Timing Oracle

**Date:** 2026-05-30  
**Severity:** Medium  
**Component:** C# — `CulpeoConnection.HandleAuthResponse`

## Description

`string.Equals(Ordinal)` short-circuits on the first differing byte. An attacker making
many auth-response attempts can use response-time distribution to recover leading bytes
of the pending nonce, potentially recovering the full nonce within its 30-second window.

The C++ implementation correctly uses `CRYPTO_memcmp`. The C# implementation does not.

## Recommendation

Replace with `CryptographicOperations.FixedTimeEquals` (BCL since .NET 5).
Also increase `NonceByteLength` from 16 to 32 bytes to match the C++ implementation.

## Status

Open
