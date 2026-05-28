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
