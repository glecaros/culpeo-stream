# SEC-010: Rate Limiter Uses Raw Socket IP — Ineffective Behind Reverse Proxy

**Date:** 2026-05-30  
**Severity:** High  
**Component:** C# — `CulpeoStreamMiddleware` / `IpRateLimiter`

## Description

`GetClientIp` returns `context.Connection.RemoteIpAddress` unconditionally. With the
default `TrustForwardedProto = true` (proxy deployment), every connection arrives from
the proxy's IP. All clients share one rate-limit bucket — 10 connections/min for the
entire user population. A single attacker can hold all 10 slots and deny service to
everyone else.

## Recommendation

When `TrustForwardedProto = true`, extract client IP from `X-Forwarded-For` (rightmost
IP from the trusted proxy segment). Expose `TrustedProxyCount` option. Or use ASP.NET
Core's `ForwardedHeaders` middleware so `RemoteIpAddress` is already rewritten.

Document explicitly: `TrustForwardedProto = true` requires `ForwardedHeadersOptions` or
rate limiting is non-functional.

## Status

Open
