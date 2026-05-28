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
