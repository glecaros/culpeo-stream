## Span-based frame parsing
**Date:** 2026-05-26
**Phase:** Phase 1 — Core Library
**Status:** Decided

### Context
Phase 1 requires allocation-efficient frame parsing while still exposing a simple transport-agnostic API. The main choice was whether to build the parser around strings and splitting, or to scan the frame bytes directly and decode only recognized header values.

### Decision
The parser scans `ReadOnlySpan<byte>` for the `\r\n\r\n` delimiter, parses header lines in-place, decodes only reserved headers, and exposes the body as a sliced `ReadOnlyMemory<byte>` without copying.

### Tradeoffs
This keeps parsing allocations low and naturally ignores unknown headers, but it makes the parser code more manual than a split-based implementation.

### Spec Reference
Section 4, Section 10.3

## Session persistence model
**Date:** 2026-05-26
**Phase:** Phase 1 — Core Library
**Status:** Decided

### Context
Session resumption needs server-side state for stream declarations, offsets, and replay bounds after a transport drop. The design question was whether to couple resumption to a live connection object or keep resumable state in a shared server store.

### Decision
The core library keeps resumable session snapshots in an in-memory server store owned by `CulpeoSessionServer`. Connections are lightweight views over that shared state and call `DisconnectAsync` to mark an unexpected transport drop.

### Tradeoffs
This cleanly models reconnection and version/init handling, but it is intentionally in-memory only for Phase 1 and does not provide distributed persistence.

### Spec Reference
Section 7.2, Section 7.4

## Single-use auth refresh nonces
**Date:** 2026-05-26
**Phase:** Phase 1 — Core Library
**Status:** Decided

### Context
The auth-refresh flow is security-sensitive. The implementation needed a nonce format, entropy source, and replay-prevention strategy that would not expose bearer tokens.

### Decision
Nonces are generated with `RandomNumberGenerator.GetBytes()`, encoded as hex, tracked in a `HashSet<string>`, and removed immediately after a successful match so they cannot be reused.

### Tradeoffs
Hex encoding is slightly larger than Base64, but it is easy to inspect in tests and keeps the nonce transport-safe. Replay attempts fail cleanly without ever echoing authorization credentials.

### Spec Reference
Addendum A.4, Addendum A.5

## Parser limit enforcement
**Date:** 2026-05-27
**Phase:** Phase 1 — Core Library
**Status:** Decided

### Context
The security review flagged that the parser had no bounds enforcement. The hardened spec (§4.1.1) mandates limits on header block size, header count, header name length, and header value length, plus CR/LF/NUL rejection (§4.1) and duplicate reserved header rejection.

### Decision
Added a `ParseLimits` class with configurable thresholds matching the spec defaults (8,192 byte block, 64 headers, 256 byte name, 4,096 byte value). The parser validates all limits before decoding, rejects forbidden bytes in names and values, and tracks seen reserved headers via `HashSet<ReservedHeader>` to detect duplicates. Unknown headers are still allowed to repeat per spec.

### Tradeoffs
Adds a small per-parse allocation for the `HashSet`, but it is bounded at 10 entries (the number of reserved headers). Configurable limits let operators raise thresholds while preserving safe defaults.

### Spec Reference
Section 4.1, Section 4.1.1

## Stream count and ping rate enforcement
**Date:** 2026-05-27
**Phase:** Phase 1 — Core Library
**Status:** Decided

### Context
The hardened spec added two resource caps that the C# implementation did not enforce: a maximum of 16 concurrent streams per session (§5.6) and a maximum ping rate of 5 per second (§6.1). Both were flagged during the security review as missing enforcement.

### Decision
Added `MaxStreamCount` (default 16) to `CulpeoSessionOptions`, enforced during stream declaration validation with an `"invalid-streams"` rejection. Added a sliding-window ping rate limiter in `CulpeoConnection` using a `Queue<DateTimeOffset>` that tracks ping timestamps and closes the connection with `"rate-limit-exceeded"` if more than 5 arrive within any 1-second window.

### Tradeoffs
The queue grows at most to the rate limit size (5 entries) before triggering rejection, so memory overhead is negligible. The stream count limit is configurable; the ping rate is hardcoded since the spec defines it as a fixed limit.

### Spec Reference
Section 5.6, Section 6.1

## Adoption of .NET 8 built-in APIs and C# 12 features
**Date:** 2026-05-27
**Phase:** Phase 1 — Core Library
**Status:** Decided

### Context
The initial implementation used hand-rolled utilities for ASCII case-insensitive comparison, byte scanning, and header terminator search. The codebase also used older C# patterns (explicit constructors, `Array.Empty<T>()`, `new[] { ... }`).

### Decision
Replaced custom code with .NET 8 built-in APIs: `System.Text.Ascii.EqualsIgnoreCase` for header name matching, `System.Buffers.SearchValues<byte>` with `IndexOfAny` for forbidden byte scanning, and `ReadOnlySpan<byte>.IndexOf` for terminator/CRLF search. Adopted C# 12 collection expressions (`[]`), primary constructors for service types and internal models, and target-typed `new()` throughout.

### Tradeoffs
Requires .NET 8+ (already the target). Removes ~60 lines of hand-rolled utility code in favor of runtime-optimized intrinsics. Primary constructors reduce boilerplate but make the captured parameters implicitly mutable fields — acceptable for internal types.

### Spec Reference
C# agent instructions Technical Requirements

---

## WebSocket transport adapter design
**Date:** 2026-05-28
**Phase:** Phase 2 — ASP.NET Core Integration
**Status:** Decided

### Context
Phase 2 requires bridging ASP.NET Core's `WebSocket` object to CulpeoStream.Core's `CulpeoConnection`. The key design question was whether to tightly couple the middleware to the Core objects or introduce an explicit adapter layer.

### Decision
Introduced `WebSocketTransportAdapter` as the sole point of integration between the WebSocket transport and Core. The adapter:
- Implements `ICulpeoStreamSession` (the public interface for application code to send outbound frames).
- Holds a `SemaphoreSlim(1, 1)` to serialise WebSocket sends, allowing the handler to call `SendMediaAsync` concurrently with the receive loop.
- Uses a per-iteration `CancellationTokenSource` created with `CancelAfter(idleTimeout)` and linked to the request cancellation token, so each receive call has a bounded deadline. No background timer thread is needed.
- Uses `ArraySegment<byte>` overload of `ReceiveAsync` (returning `WebSocketReceiveResult`) for message reassembly, matching the standard fragmentation pattern from RFC 6455 §5.4.

### Tradeoffs
A new `CancellationTokenSource` is created and disposed per receive iteration. The allocation cost is negligible compared to network I/O but is visible in profiling. The alternative (a `System.Timers.Timer`) would have required cross-thread signalling. The per-call CTS is simpler, testable, and avoids timer thread interactions.

### Spec Reference
Section 7, Addendum B, Section 3.1

---

## Rate limiting strategy: per-IP sliding window
**Date:** 2026-05-28
**Phase:** Phase 2 — ASP.NET Core Integration
**Status:** Decided

### Context
§A.5 says servers SHOULD rate-limit connection attempts to mitigate credential stuffing. The implementation needed a strategy that is simple, in-memory, and does not require an external dependency.

### Decision
Implemented a sliding-window rate limiter (`IpRateLimiter`) using a `ConcurrentDictionary<string, IpRecord>` where each `IpRecord` holds a `Queue<long>` of connection attempt timestamps (in ticks). On each `TryAcquire`, entries older than 60 seconds are evicted before the count is checked. IP keys are never pruned from the top-level dictionary (memory leak bounded to the number of distinct IP addresses seen by the process).

Window size: 60 seconds, default limit: 10 connections/minute (both configurable). Rate limiting is applied only at WebSocket upgrade time, not to subsequent frames.

### Tradeoffs
The in-memory dictionary grows with distinct IP addresses but does not shrink. For a server behind NAT this means one entry per NAT address, which is bounded and acceptable. A background cleanup task was deliberately omitted to keep the implementation simple; the leak is negligible in practice. The alternative (token bucket or fixed window) would be equally valid; sliding window was chosen because it gives the most precise "last 60 seconds" semantics.

### Spec Reference
Section A.5

---

## wss:// enforcement and reverse-proxy interaction
**Date:** 2026-05-28
**Phase:** Phase 2 — ASP.NET Core Integration
**Status:** Decided

### Context
§3.1 and §B.5 require wss:// in production. In practice many deployments terminate TLS at a load balancer, so the ASP.NET Core process receives plain ws:// traffic even for connections that were originally encrypted. The challenge was how to distinguish real plain-text connections from reverse-proxied TLS connections without coupling to any specific proxy header library.

### Decision
The middleware checks `IHostEnvironment.IsProduction()` first. In non-production environments (Development, Staging) the check is skipped entirely. In production:
1. If `TrustForwardedProto = true` (the default), inspect the `X-Forwarded-Proto` header. A value of `https` is accepted as evidence of TLS.
2. If no `X-Forwarded-Proto` header is present, fall back to `HttpContext.Request.IsHttps`.
3. If neither indicates TLS, respond with 403 Forbidden.

The `TrustForwardedProto` option defaults to `true` but is documented with a warning: enable only when a trusted reverse proxy controls this header. Operators who do not have a trusted proxy MUST set `TrustForwardedProto = false`.

### Tradeoffs
Trusting `X-Forwarded-Proto` by default is a pragmatic choice for cloud-native deployments but creates a spoofing risk if the application is directly internet-facing. This is documented in `CulpeoStreamOptions.TrustForwardedProto`. The alternative (always require `IsHttps`) would break every reverse-proxy deployment without an explicit workaround. We chose the more usable default with a clear opt-out.

### Spec Reference
Section 3.1, Section B.5, Section A.5

---

## Middleware pipeline position and ordering
**Date:** 2026-05-28
**Phase:** Phase 2 — ASP.NET Core Integration
**Status:** Decided

### Context
`MapCulpeoStream` uses endpoint routing. The question was whether to require callers to call `app.UseWebSockets()` before `app.UseRouting()`, or to inject it transparently.

### Decision
`MapCulpeoStream` creates a sub-pipeline via `endpoints.CreateApplicationBuilder()`, inserts `app.UseWebSockets()` into that sub-pipeline, and maps the resulting delegate at the specified pattern. This means callers do NOT need to call `UseWebSockets()` on the outer pipeline; it is injected automatically for the CulpeoStream endpoint only.

The `CulpeoStreamMiddleware` class itself can also be used directly via `app.UseMiddleware<CulpeoStreamMiddleware>(handler)` for cases where callers prefer explicit pipeline control. In that case the caller is responsible for calling `UseWebSockets()` before the middleware.

### Tradeoffs
Automatic `UseWebSockets()` injection is convenient but could surprise operators who have already called it globally (double-invocation is harmless but redundant). Explicit documentation in `MapCulpeoStream` mitigates this. The benefit — zero-configuration setup for the common case — outweighs the minor redundancy risk.

### Spec Reference
ASP.NET Core WebSocket middleware documentation, Addendum B
