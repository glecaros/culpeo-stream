## HTTP/2 frame envelope byte order (type-first vs length-first)
**Date:** 2026-05-27
**Phase:** Phase 4 — HTTP/2 Transport
**Status:** Decided

### Context
Spec Addendum C.4 describes the HTTP/2 framing envelope as `[4-byte-length][type-octet + payload]` (length first, then type is the first byte of the payload). The Phase 4 task instructions specify `[type:1][length:4-BE][payload:N]` (type first, then length). These two orderings are incompatible.

### Decision
Implemented `[type:1][length:4-BE][payload:N]` as stated in the task instructions. The type octet identifies the CulpeoStream frame kind (0 = control, 1 = media). The 4-byte big-endian length is the payload byte count, excluding both the type octet and the length field itself.

### Tradeoffs
This deviates from the literal Addendum C.4 wording. The task instructions are treated as authoritative for the implementation. If the spec wording is later clarified, the framing layer (`Http2FrameWriter`/`Http2FrameReader`) can be updated in isolation since it is fully encapsulated.

### Spec Reference
Addendum C.4

---

## HTTP/2 request-body streaming via `RequestBodyContent`
**Date:** 2026-05-27
**Phase:** Phase 4 — HTTP/2 Transport
**Status:** Decided

### Context
The CulpeoStream HTTP/2 transport uses a long-lived HTTP/2 POST: the request body is the client→server frame stream and the response body is the server→client frame stream. `HttpClient` sends the request body via an `HttpContent` subclass. The challenge: `ConnectAsync` must obtain the live request-body `Stream` (from inside `SerializeToStreamAsync`) AND must also wait for `SendAsync(ResponseHeadersRead)` to complete before handing the connection to the caller.

Several approaches were considered:
1. **`StreamContent(PipeReader.AsStream())`** — immediately cancelled when `ResponseHeadersRead` fires.
2. **`DuplexPipeContent` (Pipe-based)** — `SerializeToStreamAsync` pours the pipe reader into the network; writes via `PipeWriter` from outside. Never worked because `SendAsync` blocked.
3. **`RequestBodyContent` (TCS-based stream capture)** — `SerializeToStreamAsync` sets a `TaskCompletionSource<Stream>` with the raw HTTP/2 stream and then awaits a `_doneTcs` to keep the request alive. `ConnectAsync` awaits `StreamAvailableTask` after `SendAsync` returns.

### Decision
Used `RequestBodyContent` (option 3). `SerializeToStreamAsync` captures the provided `Stream` via `TaskCompletionSource<Stream>`, then awaits `_doneTcs` which is signalled only when the connection is disposed. This keeps the request body open for the full session lifetime. `DisposeAsync` on `CulpeoHttp2Connection` calls `_requestContent.Complete()` to trigger END_STREAM.

### Tradeoffs
The connection is live until `DisposeAsync` is called — no half-close on the request side. This is correct for the protocol (either party closing the connection ends the session). The TCS pattern adds a small allocation per connection but is a one-time cost.

### Spec Reference
Addendum C

---

## Kestrel h2c `SendAsync(ResponseHeadersRead)` deadlock — fix via `FlushAsync`
**Date:** 2026-05-27
**Phase:** Phase 4 — HTTP/2 Transport
**Status:** Decided

### Context
When using cleartext HTTP/2 (h2c) with Kestrel, calling `ctx.Response.StartAsync()` inside a `MapPost` endpoint handler does not immediately flush the 200 OK HEADERS frame to the wire. `HttpClient.SendAsync(request, ResponseHeadersRead)` blocks until either (a) the server endpoint handler completes, or (b) actual DATA is flushed to the response body. For a long-lived session handler this would mean `SendAsync` never returns until the session ends — making `ConnectAsync` unusable.

Approaches considered:
1. **Use TLS in tests** — ALPN h2 would not exhibit this issue, but adds cert management complexity to tests.
2. **Fire handler as `Task.Run` + keepalive TCS** — endpoint handler returns quickly, background task holds response alive.
3. **`ctx.Response.Body.FlushAsync()` after `StartAsync()`** — forces Kestrel to emit an empty DATA frame (or at minimum the HEADERS frame) to the wire, unblocking the client's `ResponseHeadersRead` completion.

### Decision
Used option 3: after `await ctx.Response.StartAsync(ct)`, immediately call `await ctx.Response.Body.FlushAsync(ct)`. This is the minimal invasive change and works reliably with Kestrel h2c. The endpoint handler continues to run for the full session duration (correct semantics — `ctx.RequestAborted` is scoped to the endpoint lifetime).

### Tradeoffs
Relies on Kestrel flushing the HEADERS frame when `FlushAsync` is called with no DATA yet written. This is documented Kestrel behavior (an explicit flush always writes buffered headers + any pending DATA). If a future Kestrel version changes this, the diagnostic test `Diagnostic_SendAsync_ReturnsOnceServerHandlerYields` will catch it. Two diagnostic tests are retained in the test suite for this reason.

### Spec Reference
Addendum C.3 (transport connection establishment)

---

## Kestrel MinRequestBodyDataRate disabled for streaming connections
**Date:** 2026-05-27
**Phase:** Phase 4 — HTTP/2 Transport
**Status:** Decided

### Context
Kestrel's default `MinRequestBodyDataRate` is 240 bytes/second with a 5-second grace period. For CulpeoStream sessions (long-lived, bursty audio streams) this would terminate connections that have been silent for more than ~5 seconds between audio frames. This is incorrect behavior for the protocol.

### Decision
In `MapCulpeoHttp2`, immediately upon entering the endpoint handler, we disable the rate limit via `IHttpMinRequestBodyDataRateFeature.MinDataRate = null`. This is done before `StartAsync()` so it takes effect for the entire request.

### Tradeoffs
Disabling the rate limit removes a denial-of-service defence against clients that open connections and then send nothing. The server should rely on its own idle-timeout mechanism (session-level ping/pong or a connection-level timeout) to close truly inactive sessions.

### Spec Reference
Section 6 (ping/pong keepalive)

---

## HTTP/2 cleartext (`Http2UnencryptedSupport`) opt-in
**Date:** 2026-05-27
**Phase:** Phase 4 — HTTP/2 Transport
**Status:** Decided

### Context
`System.Net.Http.SocketsHttpHandler` requires the process-wide `Http2UnencryptedSupport` AppContext switch to allow h2c (HTTP/2 without TLS). By default, .NET 6+ rejects cleartext HTTP/2 connections to mitigate downgrade attacks.

### Decision
`CulpeoHttp2ClientOptions.AllowCleartextHttp2` (default `false`) controls whether the client sets this switch. When `true`, `CulpeoHttp2Client.ConnectAsync` calls `AppContext.SetSwitch("System.Net.Http.SocketsHttpHandler.Http2UnencryptedSupport", true)` before connecting. The switch is process-wide and cannot be un-set; this is documented in the option's XML comment.

### Tradeoffs
The switch is process-wide and irreversible within a process lifetime. If a production application mistakenly sets `AllowCleartextHttp2 = true`, all HTTP/2 connections in the process could use cleartext. The option is named explicitly to make its security implication obvious. No compiler warning is emitted (unlike the requirement for WebSocket `ws://`) because .NET does not provide a `[Obsolete]`-based warning path for runtime switches; the option documentation serves as the warning.

### Spec Reference
Security requirements (wss:// / TLS enforcement)

---

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

## Phase 3: Client library architecture — ConnectAsync + background loop
**Date:** 2026-05-29
**Phase:** Phase 3 — Client Library
**Status:** Decided

### Context
`CulpeoStreamClient` needs to support both a synchronous initial handshake (caller waits for `culpeo.init-ack` before returning) and an ongoing background receive loop that handles reconnections, auth-refresh challenges, and incoming media/events.

### Decision
`ConnectAsync` performs the initial WebSocket connection and `culpeo.init` handshake synchronously (from the caller's perspective). On success it emits `SessionEstablished` into the event channel, starts a `Task.Run` background receive loop, and returns. The background loop drives all subsequent receives, reconnects, and auth-refresh handling. `ReceiveAsync` reads from an unbounded `Channel<CulpeoClientEvent>` that both the initial connect and the background loop write to.

### Tradeoffs
- **Pros:** Simple caller experience; clean separation between initial handshake and steady-state; the channel lets callers use `await foreach` without worrying about reconnects.
- **Cons:** The background `Task.Run` is detached from the caller's `CancellationToken` (uses the lifetime CTS instead), meaning callers must await `DisposeAsync` or `DisconnectAsync` for clean shutdown.

### Spec Reference
§7.1, §7.2, §10.4

---

## Phase 3: WebSocket factory for testability
**Date:** 2026-05-29
**Phase:** Phase 3 — Client Library
**Status:** Decided

### Context
`CulpeoStreamClient` creates a `ClientWebSocket` internally and calls `ConnectAsync` on it. Unit tests need to substitute in-memory WebSockets without a real network. Subclassing is not possible (class is `sealed`). Mocking the BCL `ClientWebSocket` with external frameworks was ruled out (no external dependencies in Core).

### Decision
Added an `internal Func<Uri, CancellationToken, Task<WebSocket>>? WebSocketFactory` property to `CulpeoStreamClient`. When non-null it replaces the default `ClientWebSocket` path. Test assemblies are granted access via `[assembly: InternalsVisibleTo("CulpeoStream.Client.Tests")]`. Tests use a hand-rolled `MemoryWebSocket` built on `System.Threading.Channels` pipes.

### Tradeoffs
- **Pros:** No external mocking library; full control over WebSocket lifecycle in tests; production code path remains unchanged.
- **Cons:** Internal surface exposed to tests via `InternalsVisibleTo`; the `MemoryWebSocket` is a non-trivial in-test abstraction but is self-contained.

### Spec Reference
§3 (transport abstraction)

---

## Phase 3: wss:// enforcement in client
**Date:** 2026-05-29
**Phase:** Phase 3 — Client Library
**Status:** Decided

### Context
§3.1 of the spec requires encrypted transports in production. The client must enforce `wss://` by default.

### Decision
`ConnectAsync` checks `serverUri.Scheme` against `"ws"` (case-insensitive) before creating the WebSocket. If the scheme is `ws://` and `AllowInsecureConnections` is `false` (the default), it throws `InvalidOperationException` immediately. A doc-comment on `AllowInsecureConnections` clearly marks it as a security risk for local development only. No compile-time warning is emitted (that would require a different API surface).

### Tradeoffs
- Plaintext connections are blocked by default, matching the security requirement.
- The `AllowInsecureConnections` opt-in flag lets integration tests and local dev work without real TLS.
- No `[Obsolete]` attribute was placed on the property itself since that would flag every use, even `false`.

### Spec Reference
§3.1, §A.5

---

## Phase 3: Full-jitter exponential backoff for reconnection
**Date:** 2026-05-29
**Phase:** Phase 3 — Client Library
**Status:** Decided

### Context
§10.4 says clients SHOULD use exponential backoff between reconnection attempts. Full-jitter is the recommended algorithm to avoid thundering-herd problems with many simultaneous reconnects.

### Decision
Used `random(0, min(MaxBackoff, InitialBackoff × 2^attempt))` where `random` draws from `RandomNumberGenerator.GetInt32`. This provides full-jitter backoff as described in the AWS Architecture Blog. `InitialBackoff` defaults to 1 s, `MaxBackoff` to 30 s, `MaxReconnectAttempts` to 10.

### Tradeoffs
- Full-jitter has lower average reconnection latency than exponential-only while distributing load more evenly.
- `RandomNumberGenerator.GetInt32` is cryptographically secure but slightly heavier than `Random.Next`. For backoff the overhead is negligible.

### Spec Reference
§10.4

---

## Phase 3: Offset tracking — per-stream send and receive cursors
**Date:** 2026-05-29
**Phase:** Phase 3 — Client Library
**Status:** Decided

### Context
§8.2 defines three offset types (`time`, `byte`, `message`) and §7.2 / §8.2 require clients to track the highest received offset per stream for resumption. The client also needs to stamp outgoing media frames with the correct offset.

### Decision
`ClientStreamState` holds two cursors per stream: `SendOffset` (used as `Offset` on outgoing frames, advanced after each `SendMediaAsync`) and `ReceiveOffset` (updated on each incoming media frame, used as `resume_offset` for output streams). For `time`-typed PCM streams the bytes-per-sample stride is precomputed from the `audio/pcm` content-type parameters at init-ack parse time.

### Tradeoffs
- Two cursors cleanly handle duplex streams where both parties send.
- PCM stride is precomputed once, keeping `SendMediaAsync` allocation-free.
- If a future spec version adds other time-based content types the helper returns a byte-fallback.

### Spec Reference
§5.5, §8.2, §7.2

---

## Phase 3: ContentTypeUtilities made public
**Date:** 2026-05-29
**Phase:** Phase 3 — Client Library
**Status:** Decided

### Context
`CulpeoStream.Client` needed to validate incoming media frame `Content-Type` against the declared stream content-type (§6.2). The matching logic already existed in `CulpeoStream.Core` as `ContentTypeUtilities` but was `internal`.

### Decision
Changed `ContentTypeUtilities` and its associated `ParsedContentType` record from `internal` to `public` in `CulpeoStream.Core`. This avoids duplication without requiring `InternalsVisibleTo` on Core.

### Tradeoffs
- Slightly wider public surface on Core; but this utility is genuinely reusable and its semantics are spec-defined, so public visibility is appropriate.
- No breaking change since the types were not previously accessible.

### Spec Reference
§6.2

## Atomics-first concurrency policy
**Date:** 2026-05-31
**Phase:** Cross-cutting
**Status:** Decided

### Context
Multiple phases introduced concurrent-access guards (ConnectAsync double-call prevention, send serialization). There was a tendency to reach for `SemaphoreSlim` as a general-purpose mutex, even for simple state flags where no async wait is needed.

### Decision
Prefer `Interlocked.CompareExchange` / `Interlocked.Exchange` for state flags and "already started" guards. Only use `SemaphoreSlim(1,1)` when the critical section genuinely spans an `await` boundary (e.g., `_sendLock` serializing frame serialization + `SendAsync`). Never use `lock` around async code.

### Tradeoffs
Atomics are non-blocking, allocation-free, and naturally compose with async code. The trade-off is that compound multi-field operations still require a lock — atomics cannot atomically update two fields simultaneously. Use the simplest correct primitive.

### Spec Reference
N/A (implementation policy)

## CS-P3-001: ConnectAsync concurrent-call serialization via _connectLock
**Date:** 2026-05-30
**Phase:** Phase 3 — Client Library (review fixes)
**Status:** Decided

### Context
Two concurrent callers of `ConnectAsync` could both pass the `if (_loopTask is not null)` check before either set it, resulting in two independent background loops running on the same client instance. The original guard was a simple null check without synchronization.

### Decision
Added `SemaphoreSlim _connectLock = new(1, 1)`. `ConnectAsync` acquires it before entering the null check and releases it in the `finally` block after setting `_loopTask`. The second concurrent caller blocks until the first completes the check, then sees a non-null `_loopTask` and throws.

### Tradeoffs
Adds one heap allocation (the semaphore) per client instance. Blocking the second caller until the first finishes its full handshake is acceptable because `ConnectAsync` is not expected to be called frequently; it is a one-shot operation per client lifetime.

### Spec Reference
N/A (implementation safety)

## CS-P3-002: Bounded event channel with configurable capacity
**Date:** 2026-05-30
**Phase:** Phase 3 — Client Library (review fixes)
**Status:** Decided

### Context
`Channel.CreateUnbounded` lets the channel grow without limit. If the consumer (`ReceiveAsync`) falls behind, the receive loop would continue writing frames, eventually consuming unbounded memory.

### Decision
Replaced `CreateUnbounded` with `Channel.CreateBounded` (capacity configurable via `CulpeoStreamClientOptions.EventChannelCapacity`, default 1024) with `FullMode = Wait`. This exerts back-pressure on the receive loop rather than silently dropping frames. `SingleWriter = true` is set because only the receive loop writes; `SingleReader = false` permits defensive use. The channel is initialized in the constructor (not as a field initializer) so it can pick up the configured capacity from options.

### Tradeoffs
A full channel blocks the receive loop. Consumers MUST drain `ReceiveAsync` continuously. This is documented in the XML doc on the property. The alternative (DropOldest/DropNewest) would silently lose frames, which is worse for a protocol where every frame carries advancing offsets.

### Spec Reference
N/A (implementation safety)

## CS-P3-003: DisposeAsync awaits loop before disposing WebSocket
**Date:** 2026-05-30
**Phase:** Phase 3 — Client Library (review fixes)
**Status:** Decided

### Context
The original `DisposeAsync` cancelled the CTS and then immediately disposed `_ws`. The reconnect loop (running concurrently) could create a new `_ws` after the dispose, leaking the new instance. The fix is to await `_loopTask` to completion before touching `_ws`.

### Decision
Moved `_ws?.Dispose()` to after `await _loopTask`. Cancelling the CTS signals the loop to stop; awaiting it ensures the loop has exited and no new WebSocket can be created. `_eventChannel.Writer.TryComplete()` is also moved to after the loop exits to avoid racing with any final channel writes inside the loop.

### Tradeoffs
`DisposeAsync` now waits for the loop to exit, which may take up to the current backoff delay (max `MaxBackoff`). This is acceptable because dispose is a shutdown operation.

### Spec Reference
N/A (implementation safety)

## CS-P3-004: SendOffset mutation inside _sendLock
**Date:** 2026-05-30
**Phase:** Phase 3 — Client Library (review fixes)
**Status:** Decided

### Context
`SendMediaAsync` read `stream.SendOffset`, built a frame, called `SendFrameAsync` (which acquired `_sendLock` internally), and then updated `stream.SendOffset` outside the lock. Two concurrent sends could both read the same offset, both build frames with that offset, and both write the same value back — producing duplicate offsets and incorrect resume state.

### Decision
The offset read, frame serialization, WebSocket send, and offset increment are all performed inside `_sendLock`. `SendFrameAsync` is no longer called from `SendMediaAsync`; instead the send is inlined to keep the atomic read-send-increment inside one lock acquisition.

### Tradeoffs
Serialization now happens inside the lock, which is slightly less parallel. However, because frames must be sent in offset order anyway, there is no benefit to serializing outside the lock. The net effect is correct, contiguous offsets under concurrent sends.

### Spec Reference
§7.2 (offset monotonicity)

## SEC-018: GetToken always called on connect and reconnect
**Date:** 2026-05-30
**Phase:** Phase 3 — Client Library (review fixes)
**Status:** Decided

### Context
Every `culpeo.init` on reconnect used the static `_options.Authorization` string. If the token had expired between the first connect and a reconnect, all reconnect attempts would fail even though `GetToken` could provide a fresh one.

### Decision
Added `GetCurrentTokenAsync(CancellationToken)` helper. When `GetToken` is non-null it is called on every invocation of `PerformHandshakeAsync` (initial connect and every reconnect). When `GetToken` is null, the static `Authorization` is used. `Authorization` is now nullable; the constructor validates that at least one of `Authorization` / `GetToken` is non-null.

### Tradeoffs
Making `Authorization` no longer `required` is a source-breaking change for any callers that relied on the compiler-enforced initializer. The validation is deferred to the constructor, which is still an immediate failure. The benefit is that short-lived tokens work correctly across reconnects without any caller changes.

### Spec Reference
§3.2 (Authorization header), §8 (session resumption)

## SEC-019: Server-supplied resume_offset validated in ProcessInitAck
**Date:** 2026-05-30
**Phase:** Phase 3 — Client Library (review fixes)
**Status:** Decided

### Context
`ProcessInitAck` applied the server-supplied `resume_offset` to stream cursors without bounds checks. A malicious or buggy server could send negative values (e.g., -1) or values far exceeding what the client has sent (e.g., `Int64.MaxValue`), corrupting the client's offset tracking.

### Decision
Added two checks in `ProcessInitAck` per stream:
1. `confirmedOffset < 0` → throws `CulpeoProtocolException("protocol-error", ...)`
2. For send streams: `confirmedOffset > existing.SendOffset` → throws `CulpeoProtocolException("protocol-error", ...)`

`CulpeoProtocolException` is caught in the reconnect loop and treated as a non-resumable failure: `_sessionId` and all stream offsets are cleared, and the next attempt will be a fresh connect.

### Tradeoffs
An overly-strict server that sends a `resume_offset` equal to the client's send offset (a legitimate confirmation of all data received) is accepted. Only values strictly greater than the tracked offset are rejected. If the server legitimately confirms less than the client sent (normal case), it is accepted and the cursor is rewound — this is the intended resumption semantics.

### Spec Reference
§8.2 (session resumption, confirmed offsets)

---

## SEC-024: uint bounds-check in Http2FrameReader
**Date:** 2026-05-28
**Phase:** Phase 4 — HTTP/2 Transport (security fix)
**Status:** Decided

### Context
The original `ReadFrameAsync` read the 4-byte big-endian length as `uint` via `BinaryPrimitives.ReadUInt32BigEndian`, then immediately cast to `int` before the bounds comparison. Any value ≥ 0x80000000 (2 GiB) became a negative `int`. A negative `int` always passes `payloadLength > maxPayloadBytes` as false (negative < any positive), bypassing the check entirely. The subsequent `new byte[payloadLength]` would then throw `OverflowException` or allocate 2 GiB depending on the runtime build.

Options considered:
1. Cast to `long` first, then compare against `(long)maxPayloadBytes`
2. Stay in `uint` for the comparison; cast to `int` only after the check is known safe

### Decision
Perform the bounds check using `uint` arithmetic: compare `rawLength > (uint)maxPayloadBytes`. Since `maxPayloadBytes` is a non-negative `int` (validated before use), `(uint)maxPayloadBytes` is safe. Only after the check passes is the value cast to `int` (safe because rawLength ≤ maxPayloadBytes ≤ int.MaxValue). Added an explicit guard that rejects negative `maxPayloadBytes` values with `ArgumentOutOfRangeException`.

### Tradeoffs
Using `uint` for the comparison is self-documenting and requires no intermediate `long` allocation. Casting to `long` would also work but adds unnecessary width. The `ArgumentOutOfRangeException` guard on `maxPayloadBytes < 0` prevents a subtle second bypass where a caller passes a negative limit.

### Spec Reference
Addendum C.4 (frame framing); SEC-024

---

## SEC-031: AllowHttp2Cleartext warning and obsolete attribute
**Date:** 2026-05-28
**Phase:** Phase 4 — HTTP/2 Transport (security fix)
**Status:** Decided

### Context
`AppContext.SetSwitch("System.Net.Http.SocketsHttpHandler.Http2UnencryptedSupport", true)` is process-wide and irreversible. The original code set it silently with only an XML doc comment as a warning. A developer could accidentally enable it in production with no runtime feedback.

Options considered:
1. Throw `InvalidOperationException` when not in development (too brittle — no reliable detection of "production")
2. `[Obsolete(error: true)]` — breaks all existing test code
3. `[Obsolete(error: false)]` + `Console.Error.WriteLine` at runtime — visible at both compile-time (warning) and runtime (stderr message)
4. `ILogger` instead of `Console.Error` — requires DI injection plumbing in a constructor that currently takes no `ILogger`

### Decision
Applied `[Obsolete(error: false)]` on `CulpeoHttp2ClientOptions.AllowHttp2Cleartext` so every call-site gets a CS0618 warning at compile time. Added `Console.Error.WriteLine` in the `CulpeoHttp2Client` constructor when the switch is activated — visible even when log infrastructure is not yet configured. Internal usages within `CulpeoHttp2Client.cs` are suppressed with `#pragma warning disable CS0618`. Test files that test the cleartext path explicitly use `#pragma warning disable CS0618` or file-level suppression.

### Tradeoffs
`Console.Error` is always available; `ILogger` would require changing the constructor signature or adding an optional `ILoggerFactory` parameter — deferred to a future DI-integration PR. `[Obsolete(error: false)]` preserves backward compatibility for test code while surfacing the warning at the call site.

### Spec Reference
§5.1 (TLS requirement); SEC-031

---

## StreamDeclaration moved from CulpeoStream.Client to CulpeoStream.Core
**Date:** 2026-05-28
**Phase:** Phase 5 — NativeAOT + Source Generators
**Status:** Decided

### Context
`StreamDeclaration` was defined in `CulpeoStream.Client`. The source generator's generated `RegisteredStreams` property returns `IReadOnlyList<StreamDeclaration>`, which must be a well-known, stable type that consuming projects can reference without pulling in the Client package.

### Decision
Moved `StreamDeclaration` (the public record with `ContentType`, `Type`, `OffsetType`, `Purpose`) to `CulpeoStream.Core`. The internal `StreamDeclaration` used for init-frame parsing in `CulpeoSession.cs` was renamed `StreamInitDeclaration` to avoid the namespace collision. `CulpeoStream.Client/StreamDeclaration.cs` now contains a `global using` type alias for backward compatibility.

### Tradeoffs
Projects that had `using CulpeoStream.Client;` to resolve `StreamDeclaration` continue to work unmodified. The Core package gains a type that logically belongs there (it's a protocol concept, not a client-specific one).

### Spec Reference
§5.2 (stream declaration in culpeo.init)

---

## NativeAOT audit result: Core libraries are reflection-free
**Date:** 2026-05-28
**Phase:** Phase 5 — NativeAOT + Source Generators
**Status:** Decided

### Context
Phase 5 required auditing `CulpeoStream.Core`, `CulpeoStream.AspNetCore`, `CulpeoStream.Client`, and `CulpeoStream.Http2` for reflection usage that would produce ILC warnings when published with `PublishAot=true`.

### Decision
Audit findings:
- **CulpeoStream.Core**: No `Type.GetType()`, `Activator.CreateInstance`, or dynamic dispatch. JSON handling uses `JsonDocument` (low-level DOM) and `Utf8JsonWriter` exclusively — both are AOT-safe. All LINQ usage operates on concrete, statically-known types. No `[RequiresUnreferencedCode]` annotations were needed.
- **CulpeoStream.Client**: Uses `Channel<T>`, `Dictionary<,>`, and IAsyncEnumerable pattern — all trim-safe. LINQ `.Select().ToList()` uses statically-known `ClientStreamState` — no dynamic dispatch.
- **CulpeoStream.AspNetCore**: DI registration via `services.TryAddSingleton(sp => ...)` — the lambda is trim-safe because no reflection is used to construct types.
- **CulpeoStream.Http2**: `HttpClient` with `Version20` — AOT-safe. No custom serialization.

No `[DynamicallyAccessedMembers]`, `[RequiresUnreferencedCode]`, or `[RequiresDynamicCode]` annotations were needed in the existing code.

### Tradeoffs
The `AotPublishTests.PublishAot_Core_ZeroIlcWarnings` test validates this claim by running `dotnet publish -p:PublishAot=true` as a subprocess and asserting zero ILC warnings from `CulpeoStream.*` source assemblies. IL3000 from the test project's own use of `Assembly.Location` in test infrastructure is excluded.

### Spec Reference
Phase 5 specification

---

## CulpeoStream.SourceGen: IIncrementalGenerator design
**Date:** 2026-05-28
**Phase:** Phase 5 — NativeAOT + Source Generators
**Status:** Decided

### Context
The source generator must produce NativeAOT-safe dispatch code for `[CulpeoStreamHandler]` classes. Key design choices:
1. **Attribute delivery**: Emit attribute definitions (`[CulpeoStreamHandler]`, `[DeclareStream]`) via `RegisterPostInitializationOutput` into the consuming compilation, rather than shipping a separate attributes assembly. This avoids a second NuGet package and keeps the generator self-contained.
2. **Attribute dependency on Core types**: `[DeclareStream]` accepts `CulpeoStreamType` and `OffsetType` parameters from `CulpeoStream.Core`. This means consuming projects must reference Core — a reasonable requirement since all users implement `ICulpeoStreamHandler` from `CulpeoStream.AspNetCore` which transitively requires Core.
3. **Generated dispatch pattern**: `OnMessageAsync` uses a switch expression (no `Dictionary<>`, no reflection). `HandleMediaAsync` uses a switch expression keyed on stream ID. Both dispatch to `protected virtual` methods with `Task.CompletedTask` defaults — developers override the methods they care about.
4. **Model equatability**: `HandlerDescriptor` and `StreamDescriptor` implement `IEquatable<T>` with field-by-field comparison so the incremental pipeline can cache and avoid unnecessary regeneration.
5. **Diagnostics location encoding**: `Location` objects (not equatable) are encoded as `(FilePath, Line, Column)` tuples in the model and reconstructed at `RegisterSourceOutput` time.

### Decision
All five choices above were implemented as described. Generated code is placed in the `partial class` in the same namespace as the handler, with `#nullable enable` and `// <auto-generated/>` headers.

### Tradeoffs
- Emitting attributes via `RegisterPostInitializationOutput` means the attribute types are `internal` in the consuming assembly — fine for application code, but would need to be `public` if users want to expose their handler attributes as part of a public API. This can be addressed with a `[CulpeoStreamHandler(PublicAttributes = true)]` option in a future version.
- `protected virtual` methods (rather than `abstract` or `partial Task`) allow zero-arg compilation without implementing every dispatch method. The tradeoff is that unhandled events silently return `Task.CompletedTask` — callers must be aware of this default.

### Spec Reference
Phase 5 specification — source generator design

---

## CulpeoStream.AotTests: test structure for AOT validation
**Date:** 2026-05-28
**Phase:** Phase 5 — NativeAOT + Source Generators
**Status:** Decided

### Context
The spec requires a `tests/CulpeoStream.AotTests/` project that (a) runs basic session smoke tests and (b) validates zero ILC warnings at publish time. Two structural options:
1. A console app that acts as both the AOT binary and the test harness.
2. An xUnit test project targeting `net9.0` with `<PublishAot>true</PublishAot>` that also contains a meta-test running `dotnet publish` as a subprocess.

### Decision
Option 2: xUnit test project on `net9.0` with `<PublishAot>true</PublishAot>`. The `AotSmokeTests` class exercises the session lifecycle under both the normal `dotnet test` runner and when published as AOT. The `AotPublishTests` class contains `PublishAot_Core_ZeroIlcWarnings` which runs `dotnet publish -p:PublishAot=true` on the same project as a subprocess and validates that no ILC warnings reference `CulpeoStream.Core`, `CulpeoStream.AspNetCore`, `CulpeoStream.Client`, or `CulpeoStream.Http2`.

The AOT test uses `AppContext.BaseDirectory` (not `Assembly.Location`) to locate the project root — `Assembly.Location` returns an empty string in AOT single-file apps and produces IL3000.

### Tradeoffs
The meta-test runs `dotnet publish` which takes ~30 seconds. It is tagged `[Trait("Category", "AotPublish")]` so it can be filtered out of fast-feedback runs. xUnit itself uses reflection internally, so the AOT binary will have xUnit-related ILC warnings — these are filtered out by limiting the search to `CulpeoStream.*` assembly paths.

### Spec Reference
Phase 5 specification — NativeAOT test project

---

## Phase 5 Review Fixes — HandleMediaAsync dispatch via hint map (Finding 1)
**Date:** 2026-05-28
**Phase:** Phase 5 — NativeAOT + Source Generators
**Status:** Decided

### Context
The generated `HandleMediaAsync` previously dispatched by matching `streamId` against the user-provided hint strings (e.g. `"audio-in"`). However, the server assigns random opaque hex IDs to streams and ignores `IdHint`; no incoming media frame ever matched a generated case arm — everything fell to `OnUnknownStreamAsync`.

### Decision
Option C/A hybrid: the generator now emits:
1. A `_culpeo_streamIdMap` dictionary (hint → server-assigned ID).
2. A `protected void CulpeoRegisterStreamIds(ICulpeoStreamSession session)` method that the handler calls from `OnConnectedAsync`. It iterates `session.Streams` and matches each to a declared stream by content type + stream type + purpose, populating the map.
3. `HandleMediaAsync` does a reverse linear scan over the (small) map to find the hint for a given server-assigned ID, then dispatches by hint via a switch expression.

This is NativeAOT-safe (no reflection), allocation-minimal (one small dictionary per session), and self-documenting.

### Tradeoffs
- Users must call `CulpeoRegisterStreamIds(session)` in `OnConnectedAsync`; forgetting it means all frames go to `OnUnknownStreamAsync`. The generated XML doc comment makes this requirement explicit.
- O(n) linear scan per frame (n ≤ 16 streams) is negligible compared to the WebSocket I/O cost.

### Spec Reference
§8.2 (media frame routing)

---

## Phase 5 Review Fixes — CULPEO004 method suffix collision (Finding 2)
**Date:** 2026-05-28
**Phase:** Phase 5 — NativeAOT + Source Generators
**Status:** Decided

### Context
Two streams whose IDs differ only in separator (e.g. `"audio-in"` and `"audio.in"`) produce the same `SafeMethodSuffix = "AudioIn"`, yielding CS0111 (duplicate method) with no helpful diagnostic.

### Decision
After building the stream list in `TransformHandler`, iterate all computed suffixes and detect first-seen vs duplicate. On collision: emit CULPEO004 (Error) naming both colliding IDs and the shared suffix. Because `hasErrors` is true, generation is suppressed — the broken partial class is never emitted.

### Tradeoffs
None significant. The check is O(n) over a small list and runs at analysis time only.

### Spec Reference
N/A (generator quality)

---

## Phase 5 Review Fixes — FQN checks for interface/attribute/type matching (Finding 7)
**Date:** 2026-05-28
**Phase:** Phase 5 — NativeAOT + Source Generators
**Status:** Decided

### Context
Simple `.Name` comparisons (`iface.Name == "ICulpeoStreamHandler"`, `attr.AttributeClass?.Name == "DeclareStreamAttribute"`, `field.Type.Name != "StreamDeclaration"`) would falsely match user-defined types in different namespaces that happen to share the same simple name.

### Decision
Replaced all three with fully-qualified display-string comparisons using `symbol.ToDisplayString()`:
- `"CulpeoStream.AspNetCore.ICulpeoStreamHandler"`
- `"CulpeoStream.Generated.DeclareStreamAttribute"`
- `"CulpeoStream.Core.StreamDeclaration"`

### Tradeoffs
None. More precise matching is always better for a source generator.

### Spec Reference
N/A (generator correctness)

---

## Phase 5 Review Fixes — JSON AOT safety in CulpeoStreamClient (Finding 4)
**Date:** 2026-05-28
**Phase:** Phase 5 — NativeAOT + Source Generators
**Status:** Decided

### Context
Three `JsonSerializer.Serialize(...)` calls in `CulpeoStreamClient.cs` used anonymous types or `object?`, which rely on reflection and are stripped by ILC/NativeAOT.

### Decision
1. Created `AuthResponseBody` and `PongBody` named records to replace the two anonymous types.
2. Created `CulpeoClientJsonContext : JsonSerializerContext` with `[JsonSerializable]` for both record types, providing source-generated serialization.
3. Changed `SendEventAsync(string eventName, object? body, ...)` to `SendEventAsync(string eventName, string jsonBody = "{}", ...)`. This matches the server-side `ICulpeoStreamSession.SendEventAsync` signature and eliminates the `object?`-polymorphic serialization. Callers now pre-serialize to JSON (breaking change, but aligned with the server API design).

### Tradeoffs
`SendEventAsync` is a public API change. Callers that passed anonymous types must now pass pre-serialized JSON. The server-side equivalent already used `string jsonBody`, so this is consistent.

### Spec Reference
Phase 5 — NativeAOT / trim safety requirement

---

## Phase 5 Review Fixes — Protocol events removed from generated OnMessageAsync (Finding 9)
**Date:** 2026-05-28
**Phase:** Phase 5 — NativeAOT + Source Generators
**Status:** Decided

### Context
The generated `OnMessageAsync` switch contained eight `culpeo.*` arms that could never be reached: the middleware handles all protocol events before calling `ICulpeoStreamHandler.OnEventAsync`, which is only invoked for application-defined (non-`culpeo.`) events.

### Decision
Removed all eight `culpeo.*` arms from the generated `OnMessageAsync` switch. Also removed the eight corresponding virtual stubs (they were dead code). Added a class-level XML doc comment explaining that protocol events are middleware-handled and will never reach `OnMessageAsync`.

### Tradeoffs
Users who had overridden the `OnCulpeoInitAsync` etc. stubs (which had no effect) will get a compile error pointing to dead code. This is the correct behaviour — the dead overrides should be removed.

### Spec Reference
§5 (session lifecycle — middleware responsibility)
