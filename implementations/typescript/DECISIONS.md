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
