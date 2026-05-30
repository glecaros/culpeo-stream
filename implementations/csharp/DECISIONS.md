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

## Explicit `offset_type` field replacing content-type inference
**Date:** 2026-05-28
**Phase:** Phase 1 — Core Library
**Status:** Decided

### Context
Prior to this change, the offset-increment behaviour was implicitly inferred from `content_type`: `audio/pcm` triggered the PCM sample-count formula; everything else incremented by 1 per frame. The spec (v0.3.0, §5.5 and §8.2) now mandates an explicit `offset_type` field on every stream declaration, removing the inference and adding a third value (`byte`) that was previously unrepresentable.

Options considered:
1. Keep content-type inference for `time` and add `offset_type` only for `byte` (opt-in) — rejected because it would leave `message` streams ambiguous and deviate from the REQUIRED spec requirement.
2. Make `offset_type` optional and infer when absent — rejected because §5.5 says REQUIRED and §5.6 says `invalid-streams` for missing values.
3. Require `offset_type` on all streams, remove all inference — chosen.

### Decision
Added a public `OffsetType` enum (`Time`, `Byte`, `Message`) to the Core library. `StreamDeclaration` and `StreamState` both carry an `OffsetType` property. `ParseInitBody` now reads the `offset_type` JSON field and throws `invalid-streams` if it is absent or unrecognised. `StreamState.AdvanceOffset` dispatches on `OffsetType`:

- `Time` → PCM sample-count formula; `audio/pcm` content type and valid `channels`/`bits`/`rate` params are still required at construction time, throwing `protocol-error` if invalid (same as before).
- `Byte` → increment by raw payload byte length (new).
- `Message` → increment by 1 (unchanged behaviour, now explicit).

`MatchesStream` (used for resumption stream-matching) was updated to compare `OffsetType` in addition to `ContentType`, `Type`, and `Purpose`. `CreateInitAckFrame` now serialises `offset_type` in each confirmed stream object.

### Tradeoffs
- Existing sessions that relied on implicit `audio/pcm → time` behaviour must now declare `"offset_type":"time"` explicitly. This is a breaking wire-format change — any client or test that omits `offset_type` will receive `invalid-streams`.
- Non-PCM streams that formerly defaulted to `message` behaviour must now declare `"offset_type":"message"` explicitly. This is intentional per the spec.
- The `byte` offset type is only meaningful for opaque binary streams where callers want byte-position semantics. It is not valid for PCM (use `time`) or structured messages (use `message`). No additional validation preventing `byte` on PCM was added, as the spec does not prohibit the combination.

### Spec Reference
Section 5.5 (offset types), Section 5.6 (stream validation rule 2 and 4), Section 8.2 (offset increment formulas)

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

## SEC-008: AuthenticateAsync hook in ICulpeoStreamHandler
**Date:** 2026-05-26
**Phase:** Phase 2 — ASP.NET Core Integration (security review)
**Status:** Decided

### Context
The original `ICulpeoStreamHandler` had no way to validate bearer tokens; any non-empty token would establish a session. Application code had no hook to reject unauthorised connections.

### Decision
Added `Task<bool> AuthenticateAsync(string authorization, CancellationToken)` to `ICulpeoStreamHandler`. In `WebSocketTransportAdapter.RunAsync`, the hook is called for every `culpeo.init` frame *before* calling `_connection.ReceiveAsync`. If it returns `false`, a `culpeo.init-error` frame with code `unauthorized` is sent and the loop exits. The Core `HandleInitialFrame` still validates that the authorization field is non-blank; the handler hook performs application-level token validation on top.

### Tradeoffs
The check is placed in the AspNetCore adapter rather than Core so that the Core library remains free of `ICulpeoStreamHandler` dependencies. Application code must now implement one more method; existing tests default to `return Task.FromResult(true)`.

### Spec Reference
§A.2 (authorization), §6.1 (init error codes)

## SEC-009: MaxMessageBytes guard in ReceiveMessageAsync
**Date:** 2026-05-26
**Phase:** Phase 2 — ASP.NET Core Integration (security review)
**Status:** Decided

### Context
`ReceiveMessageAsync` accumulated WebSocket fragments into a `MemoryStream` without any size cap, allowing a malicious client to exhaust server memory by sending an oversized message.

### Decision
Added `MaxMessageBytes` to `CulpeoStreamOptions` (default 1 MiB). The size check runs before each `accumulator.Write` call. On violation, a `protocol-error` close frame is sent and a `CulpeoProtocolException` is thrown. The new public `CulpeoProtocolException` class in `CulpeoStream.Core` is caught specifically in `RunAsync` and logged at Debug level (not Error) since it is an expected client-error path.

### Tradeoffs
1 MiB covers any realistic CulpeoStream control frame while rejecting obviously malicious payloads. Media frames are typically small; the limit can be raised per-deployment via options.

### Spec Reference
§A.5 (DoS hardening)

## SEC-010: X-Forwarded-For support for per-IP rate limiting
**Date:** 2026-05-26
**Phase:** Phase 2 — ASP.NET Core Integration (security review)
**Status:** Decided

### Context
`GetClientIp` always returned `context.Connection.RemoteIpAddress`, which collapses all clients behind a reverse proxy into a single rate-limit bucket.

### Decision
Added `TrustedProxyCount` to `CulpeoStreamOptions` (default: `0`). When `> 0`, `GetClientIp` parses the `X-Forwarded-For` header and returns the IP at index `(list.Count - TrustedProxyCount)` (rightmost client before trusted proxy hops). Falls back to `RemoteIpAddress` if parsing fails. An XML doc comment explains the security model: setting it too high allows IP spoofing; setting it too low gives all proxied clients one bucket.

### Tradeoffs
Zero by default is safe. Operators using a reverse proxy must opt in by setting `TrustedProxyCount = 1` (or the appropriate count). This is explicit and auditable. We deliberately do not enable it automatically when `TrustForwardedProto = true` to avoid surprising deployments.

### Spec Reference
§A.5

## SEC-011: Constant-time nonce comparison
**Date:** 2026-05-26
**Phase:** Phase 1 — Core Library (security review)
**Status:** Decided

### Context
`string.Equals(Ordinal)` short-circuits on the first differing character, enabling a timing oracle that allows brute-force guessing of the nonce.

### Decision
Replaced the equality check in `HandleAuthResponse` with `CryptographicOperations.FixedTimeEquals` over ASCII-encoded byte representations of both nonces. Also increased `NonceByteLength` default from 16 to 32 bytes (256 bits) to match the C++ implementation and provide a wider security margin.

### Tradeoffs
Converting strings to byte arrays allocates two small arrays per validation. This is negligible given that auth-responses are rare and infrequent. `FixedTimeEquals` requires both arrays to be the same length; a null/empty `pendingNonce` is now checked explicitly before the comparison.

### Spec Reference
§A.4 (auth-refresh), §A.6 (timing attacks)

## SEC-012: Single-pending-nonce and minimum re-issue interval
**Date:** 2026-05-26
**Phase:** Phase 1 — Core Library (security review)
**Status:** Decided

### Context
`IssueAuthRefreshAsync` could be called unconditionally, allowing callers to flood clients with challenges and overwrite a pending nonce (making the original challenge unverifiable).

### Decision
`IssueAuthRefreshAsync` now throws `InvalidOperationException("Auth refresh already pending")` if `pendingNonce is not null`. It also tracks `lastAuthRefreshIssuedAt` (distinct from `authChallengeIssuedAt` which is cleared on success/timeout) and throws `InvalidOperationException("Auth refresh issued too recently")` if the interval since the last issue is below `MinAuthRefreshIntervalSeconds` (default 30, forwarded from `CulpeoStreamOptions`). The `MinAuthRefreshIntervalSeconds` property was added to both `CulpeoStreamOptions` and `CulpeoSessionOptions`.

### Tradeoffs
The minimum interval persists across successful responses (i.e., a new challenge can only be issued 30 s after the *previous* challenge was issued, not 30 s after it was responded to). This is intentionally conservative to prevent rapid rotation attacks.

### Spec Reference
§A.4

## CS-001: Preserve original session start time across resumptions
**Date:** 2026-05-26
**Phase:** Phase 2 — ASP.NET Core Integration (security review)
**Status:** Decided

### Context
`_sessionStart` was reset to `DateTimeOffset.UtcNow` on every connection (including resumptions), causing media timestamps to restart from zero after a reconnect, breaking timestamp continuity.

### Decision
Added `DateTimeOffset SessionStartedAt { get; }` to `SessionSnapshot`, set to `TimeProvider.GetUtcNow()` in `CreateNewSession`. Resumption (`ResumeSession`) reuses the existing snapshot, so `SessionStartedAt` is naturally preserved. Added `CulpeoConnection.SessionStartedAt` as a public property exposing the snapshot value. `WebSocketTransportAdapter` now reads `_connection.SessionStartedAt ?? DateTimeOffset.UtcNow` when the session is established instead of using `DateTimeOffset.UtcNow` unconditionally.

### Tradeoffs
The change is backwards-compatible; the `??` fallback ensures correctness even if the snapshot is missing (cannot happen in practice, but provides a safe default).

### Spec Reference
§8.2 (media frame timestamp)

## SEC-015: Removal of issuedNonces HashSet
**Date:** 2026-05-26
**Phase:** Phase 1 — Core Library (security review)
**Status:** Decided

### Context
`issuedNonces` accumulated every nonce ever generated in an unbounded `HashSet`. Old nonces can never be validated once `pendingNonce` is updated or cleared, so the set only grew. The membership check in `HandleAuthResponse` was redundant with the `pendingNonce` equality check.

### Decision
Removed `issuedNonces` entirely. Auth-response validation now relies solely on `pendingNonce`; the constant-time comparison (SEC-011) ensures correctness. The `.Add()` call in `IssueAuthRefreshAsync` and the `.Remove()` call in `HandleAuthResponse` were deleted. The single-pending-nonce check introduced by SEC-012 further ensures there is never more than one active nonce to compare against.

### Tradeoffs
None. The removed code provided no security benefit and constituted an unbounded memory growth vector.

### Spec Reference
§A.4
