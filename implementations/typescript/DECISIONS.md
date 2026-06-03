## Using TypeScript + Vitest Source-First Tooling
**Date:** 2026-05-26
**Phase:** Phase 1
**Status:** Decided

### Context
Phase 1 needs a strict, runtime-agnostic core package with no runtime dependencies and a fast feedback loop for protocol work. The repository did not already contain a TypeScript toolchain under `implementations/typescript/`.

### Decision
Use `tsc --noEmit` for type validation and Vitest for tests. Keep the package source-first and ESM-first in Phase 1, with exports pointing at the TypeScript entrypoint while packaging and dual-emit distribution are deferred to a later build-focused phase.

### Tradeoffs
This keeps Phase 1 lightweight and dependency-free at runtime, but defers full CommonJS distribution artifacts until a packaging phase.

### Spec Reference
Section 3; Section 4; TypeScript agent requirements

## Splitting the Core into Parser, Registry, and Session Primitives
**Date:** 2026-05-26
**Phase:** Phase 1
**Status:** Decided

### Context
The core library must remain transport-agnostic while still enforcing session ordering, stream rules, and protocol events. A monolithic session implementation would make parser testing and offset logic harder to verify independently.

### Decision
Separate the implementation into frame parsing/serialization, stream registry and offset tracking, and explicit client/server session classes that communicate through an injected `sendFrame` callback.

### Tradeoffs
This adds a few more modules, but each protocol requirement is isolated and easier to test exhaustively.

### Spec Reference
Section 4; Section 5; Section 7; Section 8; Section 9

## Treating Resume Offsets as Frame-Start Cursors
**Date:** 2026-05-26
**Phase:** Phase 1
**Status:** Decided

### Context
The spec defines media offsets per frame and states that clients track the highest received `Offset` per stream for resumption, while PCM streams advance by sample count and encoded streams advance by one frame.

### Decision
Track resume state using the highest observed frame-start offset per stream. Outgoing offsets advance by PCM sample count or by one for encoded frames; resumption replays or confirms from the highest observed start offset.

### Tradeoffs
This follows the literal wording of the spec, but can replay the last frame boundary rather than an end-exclusive cursor if an upper layer wants duplicate suppression.

### Spec Reference
Section 6.2; Section 7.2; Section 7.5; Section 8.2

## Sanitizing Authentication Failures
**Date:** 2026-05-26
**Phase:** Phase 1
**Status:** Decided

### Context
The spec allows async token refresh and nonce-based re-authentication, and the agent instructions explicitly prohibit leaking tokens into logs, thrown errors, or diagnostics.

### Decision
Represent authentication failures with fixed safe messages, never interpolate authorization header values into errors, and generate server IDs/nonces through a crypto-backed random source abstraction.

### Tradeoffs
Operators get less raw debugging context from protocol exceptions, but secrets are not exposed by the core library.

### Spec Reference
Addendum A.4; Addendum A.5; TypeScript agent security requirements

## Enforcing Parser Limits at Frame Boundaries
**Date:** 2026-05-27
**Phase:** Phase 1
**Status:** Decided

### Context
Section 4.1 and 4.1.1 require defensive parsing for frame headers. The TypeScript parser accepted arbitrarily large header blocks, unlimited header counts, oversized names and values, repeated reserved headers, and control characters that should be rejected.

### Decision
Add a configurable `ParseLimits` surface with safe defaults for header block size, header count, header name length, and header value length. Enforce six checks during parsing: bounded CRLF-CRLF search, maximum header count, maximum header name length, maximum header value length, rejection of CR/LF/NUL in parsed names and values, and rejection of duplicate reserved headers while still allowing repeated unknown headers.

### Tradeoffs
Applications that need larger metadata envelopes must opt in with explicit limits, but the default behavior is now spec-aligned and resistant to resource abuse and ambiguous header interpretation.

### Spec Reference
Section 4.1; Section 4.1.1

## Enforcing Stream Count and Ping Rate Limits
**Date:** 2026-05-27
**Phase:** Phase 1
**Status:** Decided

### Context
The protocol caps sessions at 16 declared streams and limits ping frequency to avoid abuse. The TypeScript implementation validated stream shape and session state, but did not enforce either operational limit.

### Decision
Reject stream declarations above 16 entries by default, while allowing callers to provide an alternate `maxStreamCount` when using stream validation directly. On the server, track received ping timestamps in a sliding one-second window and close with `rate-limit-exceeded` when more than five pings arrive inside that window.

### Tradeoffs
These checks may close misbehaving peers sooner and require tests to use deterministic timestamps, but they prevent unbounded session metadata and keep control traffic within spec-defined limits.

### Spec Reference
Section 5.6; Section 6.1

## Prettier for code formatting
**Date:** 2026-05-27
**Phase:** Phase 1
**Status:** Decided

### Context
The codebase had no enforced formatting standard. As the project grows with multiple contributors and agents, inconsistent formatting creates noisy diffs and slows review.

### Decision
Added Prettier as a dev dependency with `format` (write) and `format:check` (CI) scripts. Uses Prettier defaults — no custom config file — to minimize bikeshedding.

### Tradeoffs
Adds one dev dependency and a formatting pass, but ensures consistent style across all source and test files with zero configuration.

### Spec Reference
TypeScript agent requirements

---

## Phase 2: Custom Typed Event Emitter (no EventTarget, no external library)
**Date:** 2026-05-27
**Phase:** Phase 2
**Status:** Decided

### Context
The client needs a typed event emitter for `media`, `event`, `close`, `reconnecting`, `connected`, `disconnected`, `error`, and `rtt` events. Three options were considered:
1. **EventTarget** (browser/Node built-in) — untyped, requires `CustomEvent<T>` wrappers, verbose
2. **mitt** or **tiny-emitter** — external runtime dependency, violates the zero-dep requirement for the core; would add a dependency to the client package
3. **Custom TypedEventEmitter** — zero deps, full TypeScript discriminated-union types, minimal code

### Decision
Implemented a custom `TypedEventEmitter<EventMap>` class in `src/events.ts`. It uses a `Map<key, Set<listener>>` internally, fires synchronously, and provides `on`, `off`, `once`, `emit`, and `removeAllListeners`. The generic constraint uses `extends object` (not `extends Record<string, unknown>`) to allow normal interface types without requiring index signatures.

### Tradeoffs
A custom emitter means slightly more code to maintain, but it gives us perfect TypeScript inference (discriminated-union payloads, `void` events that take no argument) with zero runtime dependencies and no coupling to Node.js EventEmitter.

### Spec Reference
TypeScript agent requirements — zero runtime dependencies in core; full type coverage

---

## Phase 2: Full-Jitter Exponential Backoff with crypto.getRandomValues()
**Date:** 2026-05-27
**Phase:** Phase 2
**Status:** Decided

### Context
The reconnect strategy needs to avoid thundering-herd after mass disconnects while still converging quickly for isolated failures. Three jitter strategies were considered:
1. **No jitter** — all clients retry at the same moment
2. **Equal jitter** — `cap/2 + random(0, cap/2)` — guarantees a minimum wait
3. **Full jitter** — `random(0, min(maxDelay, base × 2^attempt))` — lowest average delay, good spread

Additionally, `Math.random()` is prohibited by the security spec. The security requirement states all randomness must use `crypto.getRandomValues()`.

### Decision
Full jitter using `crypto.getRandomValues()`. Formula: `floor(random() × min(maxDelayMs, baseDelayMs × 2^attempt))`. Default parameters: `baseDelayMs: 1000`, `maxDelayMs: 30000`, `maxAttempts: Infinity`.

The `randomFloat()` function is injectable for testing (deterministic backoff in unit tests).

### Tradeoffs
Full jitter can produce very short delays at low attempt numbers, which is acceptable because the spread prevents coordination. The `crypto.getRandomValues()` dependency requires a modern runtime (Node.js 15+, modern browsers) — both already required by the rest of the stack.

### Spec Reference
TypeScript agent security requirements — `crypto.getRandomValues()`, never `Math.random()`

---

## Phase 2: auth-refresh While Media Frames Are In Flight
**Date:** 2026-05-27
**Phase:** Phase 2
**Status:** Decided

### Context
The spec requires the client to respond to `culpeo.auth-refresh` by calling an async token callback and sending `culpeo.auth-response` with the echoed nonce. The question is: what happens to media frames received while the async token refresh is in progress?

### Decision
Delegate entirely to the core `CulpeoClientSession`, which handles `culpeo.auth-refresh` in `handleAuthRefresh()`. The session processes frames sequentially via `session.receive()` — each call returns a Promise, but since JavaScript is single-threaded and the WebSocket `message` event is synchronous, frames are processed in arrival order. The `void session.receive(frame)` pattern means we fire-and-forget, so multiple auth-refresh + media frames can be in the microtask queue simultaneously.

This is acceptable because:
1. Media frames arriving during auth-refresh are tracked by offset and delivered normally
2. The nonce is echoed from the auth-refresh frame body (not regenerated), so there's no race on nonce generation
3. The core session's `CulpeoClientSession` handles the auth-response dispatch atomically within the `handleAuthRefresh` method

No explicit locking or queuing is added in the client layer.

### Tradeoffs
A strict sequential processing model (queue all frames until auth-response is sent) would provide stronger ordering guarantees but add complexity. The current approach is correct for the protocol because offset tracking and auth-refresh operate on orthogonal state.

### Spec Reference
Section 8.3 (auth-refresh); Addendum A.4 (token handling)

---

## Phase 2: wss:// Enforcement Implementation
**Date:** 2026-05-27
**Phase:** Phase 2
**Status:** Decided

### Context
The security spec requires wss:// by default, with ws:// allowed only via explicit opt-in with a warning.

### Decision
`validateUrl()` is called at the start of `connect()` before any state is mutated. The check is:
1. If URL starts with `wss://` (case-insensitive) → proceed silently
2. If URL starts with `ws://` and `allowInsecure: true` → emit `console.warn` with a clear message mentioning token/data exposure risk, then proceed
3. If URL starts with `ws://` without `allowInsecure` → return `Promise.reject(new Error(...))` (never throw synchronously from an async method)
4. Any other scheme → `Promise.reject(new Error(...))`

The function returns a rejected Promise rather than throwing, so all callers use a single async error channel (`await connect(...)` catches both validation and protocol errors consistently).

The `console.warn` message includes the string "insecure" and warns about token/data exposure. It does NOT include the URL itself (which could contain credentials in query params in pathological cases).

### Tradeoffs
A synchronous throw would be simpler, but rejected Promises are more consistent for an async method. The `allowInsecure` flag must be explicitly `true` (not just truthy) to prevent accidental opt-in.

### Spec Reference
TypeScript agent security requirements — wss:// enforcement


## Explicit `offset_type` Field Replacing Content-Type Inference
**Date:** 2026-05-29
**Phase:** Phase 1 (revision)
**Status:** Decided

### Context
Prior to spec v0.3.0, offset increment behaviour was inferred from `content_type`: streams with `content_type` starting with `audio/pcm` used the PCM sample-count formula, and all other streams used a per-message counter of 1. This inference was implicit and fragile — it meant that encoding-agnostic or binary stream types had no way to express a byte-offset cursor, and PCM detection relied on content-type sniffing rather than explicit declaration.

The spec now requires an explicit `offset_type` field on every stream declaration, taking one of three values:
- `time` — PCM sample-count formula (previously inferred for `audio/pcm` content types)
- `byte` — raw payload byte length per frame (new capability; useful for raw binary or chunked transfer)
- `message` — 1 per delivered frame (previously the fallback for all non-PCM streams)

### Decision
Added `OffsetType = 'time' | 'byte' | 'message'` to `types.ts` and made `offset_type: OffsetType` a required field on `StreamDeclaration`. Changed `computeOffsetIncrement` signature from `(contentType, payloadLength)` to `(offsetType, payloadLength, contentType?)`, where `contentType` is only required for `offset_type: 'time'`. Removed the implicit content-type sniffing fallback entirely. `validateStreamDeclarations` now rejects missing or unrecognised `offset_type` values with `invalid-streams`. `offset_type` is propagated through the stream registry, snapshots, confirmed stream declarations, and session resumption matching (both logical-key construction and server-side stream matching include `offset_type`).

### Tradeoffs
- **Breaking change** to `StreamDeclaration`: all callers (tests, client code, external users) must now supply `offset_type`. This is intentional and required by the spec.
- The `contentType` parameter on `computeOffsetIncrement` is now optional and only validated when `offset_type === 'time'`. This is slightly looser than requiring it unconditionally, but is correct — `byte` and `message` offsets have no dependency on content type.
- Removing implicit PCM detection eliminates a whole class of silent misconfiguration: a stream with `content_type: 'audio/pcm...'` but `offset_type: 'message'` is now valid and intentional rather than impossible.
- The `logicalKey` function in `streams.ts` includes `offset_type` so that a resumed session stream must match on all declared fields including offset semantics.

### Spec Reference
Section 5.5 (offset types), Section 5.6 (stream validation), Section 8.2 (offset increment formulas)

## TS-001: Add return after failWithClose in handleAuthResponse
**Date:** 2026-05-30
**Phase:** Phase 1 (Security Fix)
**Status:** Decided

### Context
Security review finding TS-001 identified that `handleAuthResponse` called `failWithClose` for two error conditions (missing nonce, nonce mismatch) but did not `return` after either call. Execution would continue past the error path, reaching `this.pendingAuthNonce = undefined` even after a failed/rejected auth attempt. This could mask protocol errors and leave the session in an inconsistent state.

### Decision
Added `return;` immediately after each `await this.failWithClose(...)` call in `handleAuthResponse`. Two return statements added: one for the "no pending nonce" path and one for the "nonce mismatch or empty authorization" path.

### Tradeoffs
No behaviour change for the happy path. The fix makes error paths terminal as intended. `failWithClose` already closes the session; the missing `return` only meant that further lines in the function could execute on an already-closed session.

### Spec Reference
Section 7.3 (auth-refresh challenge/response lifecycle)

## TS-002: Clean up WebSocket event listeners on reconnect
**Date:** 2026-05-30
**Phase:** Phase 2 (Security/Memory Fix)
**Status:** Decided

### Context
Security review finding TS-002 identified that `openConnection()` attached four inline arrow-function listeners (`open`, `message`, `close`, `error`) to each new WebSocket but never removed them. On reconnection, the old WebSocket and all closures it held (including references to `session`, `options`, and `this`) could not be garbage collected.

### Decision
Replaced the four inline `addEventListener` lambdas with named `const` variables (`handleOpen`, `handleMessage`, `handleClose`, `handleError`) defined within `openConnection()`. These are stored in a new instance field `this.wsHandlers` before being attached. A `cleanupWebSocket(ws)` helper method reads `this.wsHandlers` and calls `removeEventListener` for all four events, then clears `this.wsHandlers`. `cleanupWebSocket` is called at the top of `openConnection()` before the old `this.ws` is overwritten.

### Tradeoffs
The handlers are still per-connection closures (capturing the specific `ws` and `session` for that connection attempt), which is required for correctness. The stored references in `this.wsHandlers` are overwritten on each new connection, so only the most-recent set is tracked. This means `cleanupWebSocket` can only clean up the immediately previous connection, which is sufficient since each old WS is discarded one at a time during sequential reconnects.

### Spec Reference
Section 9 (reconnection and session resumption)

## TS-003: PCM time-offset calculation verified correct
**Date:** 2026-05-30
**Phase:** Phase 1 (Review Finding)
**Status:** Decided

### Context
Security review finding TS-003 flagged the PCM time-offset calculation in `offsets.ts` as potentially computing total samples across all channels rather than per-channel samples. The spec §5.5 defines time offsets as samples per channel.

### Decision
After code inspection and test verification, the existing implementation is **correct**. `parsePcmStepBytes` returns `channels * (bits / 8)`, which is the byte stride of one interleaved sample frame (all channels). Dividing `payloadLength` by this stride yields the number of sample frames, which equals samples per channel for interleaved PCM. Example: stereo 16-bit, 640 bytes → stride = 4 → 160 samples per channel. The existing test (`channels=1, bits=16, 640 bytes → 320`) also confirms correctness. No code change was made.

### Tradeoffs
No change. The finding was based on a hypothetical misreading of the formula; the actual code was already correct.

### Spec Reference
Section 5.5 (time offset type, PCM sample counting)

## TS-P3-001: ws Library for Server-Side WebSocket
**Date:** 2026-06-05
**Phase:** Phase 3
**Status:** Decided

### Context
The server package needs a WebSocket server implementation. Options: Node.js 22 built-in WebSocket server (not yet stable/available), `ws` library (mature, widely used), `uWebSockets.js` (higher performance, different API).

### Decision
Use the `ws` library (`ws@^8`). It provides a stable, well-typed WebSocket server API (`WebSocketServer`) that aligns with the browser WebSocket client API used in Phase 2.

### Tradeoffs
- `ws` is synchronous/callback-based, adding minor complexity when integrating with async handlers.
- `uWebSockets.js` would offer better throughput but has a non-standard API and less TypeScript support.
- `ws` is the obvious choice for a reference implementation.

### Spec Reference
Phase 3 requirements

## TS-P3-002: Notification Handler Fire-and-Forget Pattern
**Date:** 2026-06-05
**Phase:** Phase 3
**Status:** Decided

### Context
The core `CulpeoServerSession.onNotification` callback type is `(n: SessionNotification) => void` — it is called synchronously within `receive()` and cannot return a Promise. The server-side handler methods (`onConnected`, `onMedia`, `onEvent`) are async. This creates a mismatch.

### Decision
Use `void this.handleNotification(n)` to fire the async notification handler without awaiting it. Critical ordering concern: the `init-error` notification fires *before* `dispatch(frame)` in `sendInitErrorAndClose`. Moving `ws.close()` to *after* `session.receive()` returns (in `handleInitMessage`) ensures the init-error frame is always sent before the WebSocket is closed.

### Tradeoffs
- Application callbacks (`onMedia`, `onEvent`) run asynchronously and may execute out of order if they take different amounts of time. Acceptable for a reference implementation.
- Ordering is fully correct for session lifecycle events (init, close) because those happen within the init-message handler.

### Spec Reference
Section 4 (session lifecycle)

## TS-P3-003: Session Store Save Strategy
**Date:** 2026-06-05
**Phase:** Phase 3
**Status:** Decided

### Context
Session snapshots need to be saved to enable resumption. Options: save after every media frame (expensive), save only on disconnect (may lose recent offsets), save after init-ack + on disconnect.

### Decision
Save the snapshot twice: (1) immediately after init-ack is sent (makes the session resumable from the start), and (2) on WebSocket close (captures final offsets). This balances correctness with performance.

### Tradeoffs
- Resume offsets may be slightly stale for high-frequency media streams (no per-frame saves).
- For a reference implementation this is acceptable; production deployments can subclass or wrap ISessionStore to add periodic saves.

### Spec Reference
Section 9 (session resumption)

## TS-P3-004: maxMessageBytes via ws maxPayload
**Date:** 2026-06-05
**Phase:** Phase 3
**Status:** Decided

### Context
The spec requires enforcing a maximum message size to prevent memory exhaustion attacks.

### Decision
Pass `maxPayload: options.maxMessageBytes ?? 1_048_576` to `WebSocketServer`. The `ws` library enforces this limit automatically, terminating connections that exceed it with WebSocket close code 1009. No application-level checking needed.

### Tradeoffs
- Enforcement happens at the ws layer before our code sees the message — clean, no parsing required.
- The default 1 MiB limit is conservative; real deployments may need tuning.

### Spec Reference
Security requirements; WebSocket framing limits

## TS-P3-005: JsonObject for sendEvent/onEvent Body Type
**Date:** 2026-06-05
**Phase:** Phase 3
**Status:** Decided

### Context
The task spec defines `sendEvent(eventName: string, body: unknown)` and `onEvent(..., body: unknown)`. Using `unknown` in TypeScript strict mode requires casts before calling the core `SessionBase.sendEvent()` which takes `JsonObject`.

### Decision
Use `JsonObject` (from `culpeostream`) for `IServerSession.sendEvent` and `ICulpeoStreamHandler.onEvent` body parameters. This is more precise and avoids unsafe casts. The spec's use of `unknown` was a simplified interface description.

### Tradeoffs
- Callers must construct JSON-serializable bodies, which is the correct protocol constraint.
- Slightly more opinionated than `unknown`, but avoids runtime errors from non-serializable values.

### Spec Reference
Section 6 (application event frames)

## TS-P3-001 — WebSocket listener teardown on connection close
**Date:** 2026-05-31
**Phase:** Phase 3 (review fixes)
**Status:** Decided

### Context
`ServerConnection` registered `message`, `close`, and `error` listeners on the `ws.WebSocket` in its constructor but never explicitly removed them in `handleWsClose()`. Even after the socket reaches the `CLOSED` state, the `ws` library's internal listener arrays hold references to the closures, preventing garbage-collection of the `ServerConnection` instance and its associated session state.

### Decision
At the end of `handleWsClose()`, after calling `onClose()`, explicitly call `this.ws.removeAllListeners('message')`, `this.ws.removeAllListeners('close')`, and `this.ws.removeAllListeners('error')`.

### Tradeoffs
Has no effect on correctness (the socket is already closed by this point) but ensures deterministic GC of per-session state. Small extra code; zero risk.

### Spec Reference
N/A (implementation quality / memory management)

## TS-P3-002 — Handler errors surface via optional onError callback
**Date:** 2026-05-31
**Phase:** Phase 3 (review fixes)
**Status:** Decided

### Context
Previously, exceptions thrown by `handler.onMedia` and `handler.onEvent` were silently swallowed with a comment. Application developers had no way to observe these errors without patching the handler themselves.

### Decision
Add optional `onError?(session, error): Promise<void>` to `ICulpeoStreamHandler`. In the `media` and `application-event` catch blocks of `handleNotification()`, call `handler.onError` if provided, or fall back to `console.warn`. The session is never closed as a result — handler errors are non-fatal.

### Tradeoffs
Opt-in design keeps existing handlers valid (backward compatible). The fallback `console.warn` ensures operator visibility when `onError` is absent. Auth tokens must not appear in the thrown error — documented.

### Spec Reference
N/A (server API surface)

## TS-P3-003 — LRU eviction replaces insertion-order eviction in InMemorySessionStore
**Date:** 2026-05-31
**Phase:** Phase 3 (review fixes)
**Status:** Decided

### Context
The original store evicted the oldest-inserted entry when at capacity. An active high-traffic session inserted early could be evicted while recently-established but idle sessions were kept, violating the principle of least-recently-used fairness.

### Decision
Replace insertion-order eviction with LRU: add a `lastAccessedAt` field (monotonic integer counter, not `Date.now()` — avoids ms-precision ties) to each `StoredEntry`; increment it on every `save()` and `load()`; evict the entry with the lowest counter when at capacity. Emit a `console.warn` on eviction to alert operators.

### Tradeoffs
O(n) scan per eviction — acceptable for the expected session count (≤1000 default). Using a counter instead of timestamps avoids clock-skew and sub-millisecond collisions. A proper LRU cache with O(1) eviction (doubly-linked list + hash map) is not warranted at this scale.

### Spec Reference
N/A (server implementation quality / SEC-022)

## SEC-020 — Authenticate callback receives sessionId for ownership verification
**Date:** 2026-05-31
**Phase:** Phase 3 (review fixes)
**Status:** Decided

### Context
Any authenticated user could resume an arbitrary session by supplying a valid token plus a known session ID. The `authenticate(authorization)` callback had no way to verify ownership because it did not receive the session ID being resumed.

### Decision
Change the `authenticate` option signature to `(authorization: string, sessionId?: string) => Promise<boolean>`. The server passes `frame.headers.sessionId` (which may be `undefined` for new sessions) as the second argument. JSDoc documents that implementations SHOULD verify ownership when `sessionId` is provided.

### Tradeoffs
The change is backward-compatible (second parameter is optional). Existing handlers that ignore it are still valid. Full enforcement requires the application layer to implement the ownership check — this cannot be done in the library without opaque token introspection, which would violate the security invariant of not logging tokens.

### Spec Reference
Section 7 (session resumption); SEC-020

## SEC-021 — Per-session rate limit on auth-refresh challenges
**Date:** 2026-05-31
**Phase:** Phase 3 (review fixes)
**Status:** Decided

### Context
`ServerSessionImpl.requestAuthRefresh()` forwarded every call directly to the core session with no rate limiting. A buggy timer or adversarial server code could flood a client with challenges, causing excessive token-refresh work on the client side.

### Decision
Add `minAuthRefreshIntervalMs?: number` to `CulpeoServerOptions` (default: `30_000`). `ServerSessionImpl` tracks `lastAuthRefreshAt` (epoch ms) and silently drops calls that arrive within the cooldown window. When `minAuthRefreshIntervalMs` is set to `0`, rate limiting is disabled.

### Tradeoffs
Silently dropping is preferred over throwing because the caller (typically a timer) should not need to handle the error. A warning log was considered but omitted to avoid log noise in normal operation; the rate-limit is a soft cap, not an error condition.

### Spec Reference
Section 8.3 (auth-refresh); SEC-021

## SEC-022 — LRU eviction warning; per-identity quotas documented
**Date:** 2026-05-31
**Phase:** Phase 3 (review fixes)
**Status:** Decided

### Context
FIFO eviction in InMemorySessionStore could allow a high-volume client to fill the store and evict all other identities' sessions. Full per-identity quota enforcement requires `SessionSnapshot` to carry an `identity` field and the auth callback to surface the identity — both cross-cutting changes to the core package.

### Decision
Implement the minimal viable fix: LRU eviction (see TS-P3-003) reduces but does not eliminate the DoS window; a `console.warn` is emitted on every eviction to alert operators; and `maxSessions` JSDoc advises setting it to `(expected_peak_concurrent_sessions × safety_factor)`. Full per-identity quotas require a custom `ISessionStore` implementation, documented in code.

### Tradeoffs
Does not fully prevent a single identity from monopolising the store. Accepted because full identity tracking requires changes to the core protocol types and the authenticate callback contract, which would be a larger breaking change. Documented in DECISIONS.md and code comments so a custom store author has clear guidance.

### Spec Reference
N/A; SEC-022

## HTTP/2 Transport Package Structure
**Date:** 2025-07-14
**Phase:** Phase 4
**Status:** Decided

### Context
Phase 4 requires a new `culpeostream-http2` package implementing the Addendum C binding. The package must share no runtime dependencies with the server's `ws` transport and must be usable independently. Design options were: (a) add HTTP/2 support to `culpeostream-server`, or (b) create a separate package.

### Decision
Create a standalone `packages/culpeostream-http2/` package with its own `package.json`, `tsconfig.json`, and Vitest configuration. It depends on `culpeostream` (core) for frame serialization but has zero runtime dependencies beyond Node.js built-ins (`node:http2`).

### Tradeoffs
Extra package means one more `npm install` step, but the transport-agnostic design goal demands clean separation. Sharing the `ISessionStore` and `ICulpeoStreamHandler` interfaces as a future dependency on `culpeostream-server` is deferred (they would add a circular dependency risk since both are siblings).

### Spec Reference
Addendum C; TypeScript agent Phase 4 requirements

## HTTP/2 Frame Envelope Byte Order
**Date:** 2025-07-14
**Phase:** Phase 4
**Status:** Decided

### Context
The CulpeoStream spec (Addendum C, §C.4) shows the frame envelope as `[4-byte length][type octet + payload]` — i.e. the length prefix comes first and includes the type octet in the payload. The task interface specifies `[type: 1][length: 4 big-endian][payload: N]` — type comes first, and the length covers only the payload (not the type octet).

### Decision
Follow the task interface specification: `[type:1][length:4][payload:N]`. The `encodeFrame` / `decodeFrame` functions use this layout. Both client and server use the same functions, so all CulpeoStream-HTTP/2 traffic within this implementation is consistent regardless of the spec ordering discrepancy.

### Tradeoffs
Deviates from the literal byte diagram in Addendum C §C.4. However, since this is a TypeScript-internal transport and interop is validated against the same codebase, the wire format is self-consistent. A note is left in `framing.ts` and here for future implementers to reconcile.

### Spec Reference
Addendum C §C.3, §C.4

## HTTP/2 h2c Cleartext Warning
**Date:** 2025-07-14
**Phase:** Phase 4
**Status:** Decided

### Context
Spec §C.5 says HTTP/2 MUST use TLS and MAY use h2c for local development only. The task requires `allowInsecure` support for CI tests.

### Decision
`CulpeoHttp2Server` constructor emits `console.warn` whenever `allowInsecure: true` is set, mirroring the pattern used in `CulpeoStreamClient` for `ws://` connections. TLS is enforced by default: omitting `cert`/`key` without `allowInsecure` throws a constructor-time `Error` (fail-fast, not fail-late).

### Tradeoffs
A constructor-time throw means servers are never accidentally started without TLS in production. The warn on every construction (not just first) ensures it appears in CI logs.

### Spec Reference
Addendum C §C.5

## AsyncIterable Frame Iterator Design
**Date:** 2025-07-14
**Phase:** Phase 4
**Status:** Decided

### Context
HTTP/2 streams emit `data` events. Converting these to an `AsyncIterable` requires bridging Node.js event emitters to async generators. Options: (a) `Readable.from()` (Node.js stream utility), (b) hand-rolled event-to-async-iterator bridge, (c) `async function*` generator with `stream[Symbol.asyncIterator]()`.

### Decision
Implement a hand-rolled bridge in both `client.ts` and `server.ts` using a shared reassembly buffer, a pending-resolve/reject pair, and an internal queue. This avoids converting the `Http2Stream` to a `Readable` (which changes backpressure semantics) and keeps full control over teardown.

### Tradeoffs
More code than `Readable.from()`, but no hidden buffering or backpressure surprises. The iterator's `return()` method sets `done = true`, preventing the iterator from blocking forever if the consumer breaks out of the loop early.

### Spec Reference
N/A; implementation detail

## SEC-026: Wrapping decodeFrame RangeError in Client frames() Iterator
**Date:** 2025-07-14
**Phase:** Phase 4
**Status:** Decided

### Context
`decodeFrame` throws `RangeError` when a frame's length prefix exceeds `maxPayloadBytes`. In the client's `frames()` async iterator the `data` handler called `flush()` — which calls `decodeFrame` — without a `try/catch`. An oversized frame (e.g. length field = 0xFFFF_FFFF) would escape the event listener and hit Node.js's uncaught exception handler, crashing the process. The server already had a correct `try/catch` pattern; the client was inconsistent.

### Decision
Wrap the `flush()` call in the client `data` handler with `try/catch`. On any error, call `stream.destroy(err)` — this emits the `'error'` event, which the existing error handler catches and surfaces as a rejection on the next `next()` call. Also added `let error: unknown = null` state and updated `next()` to `Promise.reject(error)` when `done && error !== null`, matching the server's pattern.

### Tradeoffs
`stream.destroy(err)` sends `RST_STREAM` to the peer, which is visible as `ERR_HTTP2_STREAM_ERROR` on the server side. In tests using a raw server, we suppress this with a no-op `stream.on('error', () => {})`. The tradeoff is acceptable: proper error propagation to the consumer is more important than a clean teardown from a malformed frame.

### Spec Reference
SEC-026 (security finding); §C.5 (transport error handling)

## SEC-027: TLS rejectUnauthorized=false Warning in Client Constructor
**Date:** 2025-07-14
**Phase:** Phase 4
**Status:** Decided

### Context
`CulpeoHttp2Server` already emits `console.warn` when `allowInsecure: true`. The client silently accepted `rejectUnauthorized: false` with no indication that TLS certificate verification was disabled, creating a security asymmetry and no audit trail.

### Decision
Emit `console.warn` once in the `CulpeoHttp2Client` constructor (not per `connect()` call) when `rejectUnauthorized === false`. Warning text is consistent with the server's allowInsecure warning.

### Tradeoffs
Warning once at construction (not per reconnect) avoids log spam but means the warning will not appear if the options object is assembled lazily. Accepted: construction is the canonical point for option validation.

### Spec Reference
SEC-027 (security finding)

## SEC-030: Content-Type Validation on Server
**Date:** 2025-07-14
**Phase:** Phase 4
**Status:** Decided

### Context
The server accepted any POST request regardless of `Content-Type`. A sender not speaking CulpeoStream (e.g. a misconfigured proxy, a fuzzer) would have its data forwarded to the application handler, potentially causing parse errors or unexpected handler behaviour. Rejecting at the HTTP layer is cheaper and clearer.

### Decision
Check `headers['content-type']` before dispatching to the handler. Respond HTTP 415 (Unsupported Media Type) and `stream.end()` if the header is absent or does not include `application/culpeostream`. Using `String.includes()` allows for content-type parameters (e.g. `application/culpeostream; version=1`). The client already sends `application/culpeostream`; no client changes were required.

### Tradeoffs
Strict content-type checking may reject proxies that strip or normalise headers. Accepted: such proxies are not expected in a CulpeoStream deployment, and failing fast is preferable to silently processing garbage.

### Spec Reference
SEC-030 (security finding); §C.2

## SEC-032: Unknown Type Octet Rejection
**Date:** 2025-07-14
**Phase:** Phase 4
**Status:** Decided

### Context
`decodeFrame` was type-agnostic — it decoded and returned any type octet, including undefined values like `0x99`. Application code received frames it did not understand and had to defensively check the octet. The spec says implementations MUST close with a protocol error on unknown type octets.

### Decision
Add a type octet check inside both the client and server `flush()` helpers (the inner loops that consume the reassembly buffer). If `typeOctet !== CONTROL_FRAME && typeOctet !== MEDIA_FRAME`, throw an `Error` naming the hex value. In the client this feeds into the SEC-026 `try/catch` → `stream.destroy(err)` path. In the server it feeds into the existing `try/catch` → `done = true, reject(err)` path.

### Tradeoffs
Placing the check in `flush()` rather than `decodeFrame` keeps `framing.ts` transport-agnostic (it has no concept of valid type octets in its current role as a generic envelope codec). The cost is duplicated logic in two files; the benefit is that both client and server enforce the constraint at the point where they decide what to do with a frame.

### Spec Reference
SEC-032 (security finding); §3.1 (frame type enumeration)

---

## Phase 5 — WASM Parser Backend: Architecture Decision
**Date:** 2025-07-14
**Phase:** Phase 5
**Status:** Decided

### Context
Phase 5 requires a WebAssembly-accelerated header parser/serializer for the hot binary-frame path, with transparent fallback to the existing pure-TypeScript implementation when WASM is unavailable (SSR, old runtimes, load failure, Emscripten not compiled).

The core design question: how deep should the WASM integration go?  Options were:
1. Replace the entire `parseFrame`/`serializeFrame` API with WASM.
2. Replace only the raw text-processing step (finding `\r\n\r\n`, splitting `key: value` lines, assembling the header byte sequence) and keep all semantic validation (limits, forbidden chars, required headers) in TypeScript.
3. Make WASM an optional entirely separate package with no hooks into the core.

### Decision
Option 2 was chosen.  A `ParserBackend` interface is added to `culpeostream` core (`src/parser-backend.ts`) with two operations:

- `parseHeaders(buf)` → ordered `[key, value]` pair array + body offset (or `null`/throws)
- `serializeFrame(headers, body)` → `Uint8Array`

`setParserBackend(backend | null)` registers an override.  All semantic validation (header count, name/value length, forbidden characters, duplicate reserved headers) is enforced in TypeScript *after* the backend produces raw pairs.  The default is `null` (pure-TS path, no behaviour change for existing callers).

A new package `packages/culpeostream-wasm/` wraps the Emscripten-compiled C code and, after a successful `initWasm()`, installs itself via `setParserBackend()`.

### Tradeoffs
- **Gave up**: uniform WASM acceleration of the full semantic pipeline; duplicate-reserved-header detection relies on the ordered pair list (works correctly since the backend returns all pairs including duplicates).
- **Gained**: all spec MUST validations remain in TypeScript (auditable, testable, no C complexity); WASM accelerates only the hot text-manipulation path; a clean plug/unplug API that is easy for the Security Agent to review.
- **Risk accepted**: the TypeScript validation loop runs a second pass over headers already processed by C.  For typical frames (< 10 headers) this overhead is negligible.

### Spec Reference
TypeScript agent Phase 5 requirements; §3 (frame format)

---

## Phase 5 — ParserBackend Interface Uses Ordered Pairs, Not Record
**Date:** 2025-07-14
**Phase:** Phase 5
**Status:** Decided

### Context
The `ParserBackend.parseHeaders` return type could be `Record<string, string>` (simple, but loses duplicates and order) or `ReadonlyArray<readonly [string, string]>` (preserves order and duplicates, enabling duplicate-reserved-header detection in TypeScript).

The public `culpeostream-wasm` package API (for direct consumers, not the core integration) uses `Record<string, string>` as specified in the Phase 5 requirements — duplicate info is not needed there since application code typically queries headers by name.

### Decision
- **`ParserBackend` interface** (core integration): uses `ReadonlyArray<readonly [string, string]>` to preserve order and allow duplicate detection.
- **`culpeostream-wasm` public API** (`parseHeaders()` / `serializeFrame()`): uses `Record<string, string>` for ergonomic consumer use.

The wasm-loader's `createWasmParserBackend()` returns a `ParserBackend` (pair list API); the module-level `parseHeaders()` converts to `Record<string, string>` before returning to callers.

### Tradeoffs
Two representations adds a small conversion cost (one `Object.entries` / `Object.fromEntries` loop per call).  Accepted: this is dwarfed by the WASM call overhead itself.

### Spec Reference
TypeScript agent Phase 5 requirements

---

## Phase 5 — Stub dist Artefacts Committed to Repository
**Date:** 2025-07-14
**Phase:** Phase 5
**Status:** Decided

### Context
Emscripten is not installed in the devcontainer.  The `dist/` folder needs *some* content for the TypeScript `import("../dist/culpeo_parser.js")` in `wasm-loader.ts` to resolve without a build step in CI.

Options:
1. Add `dist/` to `.gitignore` and require a build step before tests run.
2. Commit a stub `dist/culpeo_parser.js` that presents the Emscripten module shape but returns a sentinel (`__culpeoStub: true`) so `initWasm()` resolves `false`.
3. Bundle the compiled `.wasm` via `base64` in a TypeScript file.

### Decision
Option 2.  A stub `dist/culpeo_parser.js` is committed that:
- Exports `createCulpeoParserModule()` (same export name as the real Emscripten glue)
- Returns an object with `__culpeoStub: true`
- `initWasm()` detects the sentinel and returns `false`

A 8-byte stub `dist/culpeo_parser.wasm` (magic bytes + version only, no sections) is also committed.

When Emscripten *is* available (`make wasm` or the Docker one-liner in `WASM_BUILD.md`), the real `.js` and `.wasm` files replace the stubs and `initWasm()` returns `true`.

### Tradeoffs
- CI always runs the TypeScript fallback path (not WASM); WASM correctness can only be verified after a real Emscripten build.  This is documented explicitly in `WASM_BUILD.md`.
- Stub files must be kept in sync with the real Emscripten module shape (same export name, same factory function signature).

### Spec Reference
TypeScript agent Phase 5 requirements

---

## Phase 5 — Emscripten Version Target: 3.1.x
**Date:** 2025-07-14
**Phase:** Phase 5
**Status:** Decided

### Context
Emscripten has historically had breaking changes in how it modularises output (`MODULARIZE=1`), memory growth flags, and exported runtime methods.

### Decision
Target **Emscripten 3.1.x** (specifically 3.1.74 in Docker image `emscripten/emsdk:3.1.74`).  The build flags used are:
- `-s WASM=1` — emit WASM output
- `-s MODULARIZE=1 -s EXPORT_NAME=createCulpeoParserModule` — ES-module-compatible factory function
- `-s ALLOW_MEMORY_GROWTH=1` — allow heap expansion for large frames
- `-s EXPORTED_RUNTIME_METHODS=["ccall","cwrap","HEAPU8","getValue","setValue"]` — minimal runtime surface

### Tradeoffs
Pinning to 3.1.x means we may need to update the flags if a future Emscripten release deprecates any of them.  Accepted; the Makefile and CMakeLists.txt document the exact flags.

### Spec Reference
TypeScript agent Phase 5 requirements

---

## Phase 5 — WASM Memory Management: All Malloc/Free in TypeScript Wrapper
**Date:** 2025-07-14
**Phase:** Phase 5
**Status:** Decided

### Context
The Emscripten-compiled WASM module exposes `_malloc` and `_free`.  Two ownership models were considered:
1. C allocates output buffers and TypeScript frees them.
2. TypeScript allocates all buffers (input and output), passes pointers to C, TypeScript frees everything.

### Decision
Option 2: **TypeScript owns all allocations**.  For every call:
- TypeScript calls `_malloc(size)` for input data, header struct array, and output buffer.
- Data is copied into WASM heap via `HEAPU8.set`.
- The C function is called with pointers.
- TypeScript reads results directly from `HEAPU8`.
- TypeScript calls `_free` on all pointers in a `finally` block, ensuring no leaks even on error paths.

No WASM heap memory escapes a single call.

### Tradeoffs
- **Gave up**: the possibility of zero-copy input (we always copy `buf` into WASM heap).
- **Gained**: simple ownership model with no C-side allocation; `finally`-guaranteed freeing prevents leaks; C code stays stateless with no hidden global heap state.
- The extra copy cost is dominated by the WASM call overhead for typical frame sizes (< 2 KB headers).

### Spec Reference
TypeScript agent Phase 5 requirements; security requirements (no data persists on WASM heap between calls)
