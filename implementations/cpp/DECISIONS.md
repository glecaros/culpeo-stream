## Zero-copy frame views
**Date:** 2026-05-26
**Phase:** Phase 1 — Frame Layer
**Status:** Decided

### Context
Phase 1 requires a parser that can inspect control and media frames without copying header values or the body. The transport layer already owns the receive buffer, so copying header strings into parser-owned storage would add avoidable latency and allocations.

### Decision
`libculpeo-message` returns `ParsedHeadersView`, which stores `std::string_view` slices into the caller-owned frame buffer for the header block, reserved header values, and body. The public header documents this through the view-based API surface rather than owning strings.

### Tradeoffs
Callers must keep the underlying frame buffer alive while using parsed views. This pushes a lifetime requirement onto users of the API, but it removes heap traffic from the parser hot path and keeps binary payload access zero-copy.

### Spec Reference
Section 4, Section 4.1, C++ agent instructions Phase 1

## Parser error handling with std::expected
**Date:** 2026-05-26
**Phase:** Phase 1 — Frame Layer
**Status:** Decided

### Context
The frame parser must be usable in performance-sensitive paths and the agent instructions explicitly prohibit exceptions in the parser. The library still needs precise failure reporting for malformed headers and content types.

### Decision
The parser and content-type helpers return `std::expected<T, culpeo::message::Error>`. `Error` is a compact enum that covers malformed header lines, invalid names or values, size-limit violations, duplicate reserved headers, invalid content types, and serialization buffer exhaustion.

### Tradeoffs
An enum error type keeps the ABI and code path simple, but it does not capture source offsets or rich diagnostics. Higher layers can map these stable error codes to transport-specific logging or protocol close reasons.

### Spec Reference
Section 4.1, Section 10.1, C++ agent instructions Technical Requirements

## Header limit enforcement
**Date:** 2026-05-26
**Phase:** Phase 1 — Frame Layer
**Status:** Decided

### Context
The security requirements call out overlong headers, missing terminators, embedded CRLF in values, and null bytes in names. The parser needs deterministic bounds before it trusts or buffers a header block.

### Decision
The parser enforces a default maximum header block size of 8 KiB and a per-header-value limit of 4 KiB through `ParseLimits`. If the `\r\n\r\n` terminator is absent before the configured limit, parsing fails with `header_block_too_large` or `missing_header_terminator` instead of scanning unbounded input.

### Tradeoffs
A fixed default can reject unusually metadata-heavy frames that another deployment might prefer to accept. Exposing `ParseLimits` lets integrators raise the limit deliberately while preserving safe defaults for the core library.

### Spec Reference
Section 4, Section 4.1, C++ agent instructions Security Requirements

## Compiled frame library over header-only
**Date:** 2026-05-26
**Phase:** Phase 1 — Frame Layer
**Status:** Decided

### Context
The agent instructions say a header-only option is acceptable if feasible. The initial implementation includes parsing helpers, serialization logic, and a fuzz target, all of which benefit from a single compiled translation unit.

### Decision
Phase 1 ships `libculpeo-message` as a normal compiled CMake target instead of a header-only library.

### Tradeoffs
Consumers pay one extra library build step, but compile times stay lower, internal helpers remain private, and sanitizer/fuzzer builds have a cleaner single-entry implementation file.

### Spec Reference
C++ agent instructions Technical Requirements

## Catch2 for unit testing
**Date:** 2026-05-27
**Phase:** Phase 1 — Frame Layer
**Status:** Decided

### Context
The initial Phase 1 implementation shipped with a hand-rolled test harness using raw assertions and a manual test runner. While functional, this limits test discovery, reporting, and integration with CI tooling.

### Decision
Migrated to Catch2 v3.8.0 fetched via CMake `FetchContent`. Tests use `Catch2::Catch2WithMain` (no custom main) and `catch_discover_tests()` for automatic CTest registration. Test cases use `TEST_CASE`/`SECTION`/`REQUIRE`/`CHECK` macros.

### Tradeoffs
Catch2 adds a build-time dependency fetch and increases compile time for the test target. In return, we get structured test output, automatic test discovery, rich assertion diagnostics, and a widely-adopted framework that contributors already know.

### Spec Reference
C++ agent instructions Technical Requirements

## Hierarchical CMake structure
**Date:** 2026-05-27
**Phase:** Phase 1 — Frame Layer
**Status:** Decided

### Context
The original build used a single monolithic `CMakeLists.txt` at the `implementations/cpp/` root that defined the library, test, and fuzz targets together. As more libraries are added in later phases (e.g., `libculpeo-session`), a flat file would become unwieldy.

### Decision
Split into a top-level `CMakeLists.txt` that sets project-wide settings and calls `add_subdirectory()`, and a per-library `libculpeo-message/CMakeLists.txt` that owns its own targets, dependencies, and tests. Include paths use generator expressions (`$<BUILD_INTERFACE:...>`) to support both in-tree and installed usage.

### Tradeoffs
Adds one more CMake file per library, but each library is self-contained and can be built independently. New Phase 2+ libraries slot in with a single `add_subdirectory()` line.

### Spec Reference
C++ agent instructions Technical Requirements

## Phase 2: Session-layer thread-safety strategy
**Date:** 2026-05-28
**Phase:** Phase 2 — Session Layer
**Status:** Decided

### Context
The agent spec requires concurrent send/receive from separate threads. Options considered:
1. Single `std::mutex` protecting all session state
2. `std::shared_mutex` with shared reads for hot paths
3. Lock-free state machine with `std::atomic`

### Decision
Single `std::mutex` (`impl_->mutex`) protecting all session state. The mutex is acquired at the start of each public method and released **before** any transport I/O. Callbacks are invoked without the mutex held to prevent deadlock if the callback re-enters the session.

### Tradeoffs
A coarse lock simplifies reasoning about state consistency and eliminates deadlock potential from re-entrant callbacks. The downside is that concurrent `send_media` calls from multiple threads are serialized. In practice the lock is held only for state validation and offset arithmetic (< 1µs), making contention negligible for real workloads. The transport's own buffering ensures the actual I/O cost isn't under the lock.

### Spec Reference
C++ agent instructions Technical Requirements

## Phase 2: Nonce generation approach
**Date:** 2026-05-28
**Phase:** Phase 2 — Session Layer
**Status:** Superseded

### Context
The spec (§A.5) and agent instructions require CSPRNG nonces. Options: `RAND_bytes` (OpenSSL), `getrandom` syscall, `std::random_device`.

### Decision
~~Use `RAND_bytes(buf, 32)` from OpenSSL.~~ Superseded by "Remove OpenSSL dependency" decision below.

### Spec Reference
Section A.4, A.5, agent instructions Security Requirements

## Phase 2: Nonce comparison timing safety
**Date:** 2026-05-28
**Phase:** Phase 2 — Session Layer
**Status:** Superseded

### Context
Comparing the echoed nonce in `culpeo.auth-response` against the stored nonce must not leak timing information (oracle attack on nonce guessing).

### Decision
~~Use `CRYPTO_memcmp` from OpenSSL.~~ Superseded by "Remove OpenSSL dependency" decision below.

### Spec Reference
Section A.4, A.5, agent instructions Security Requirements

## Phase 2: Maximum stream count
**Date:** 2026-05-28
**Phase:** Phase 2 — Session Layer
**Status:** Decided

### Context
The spec §5.6 says the default maximum MUST NOT exceed 16 streams. The implementation must reject `culpeo.init` frames that exceed this.

### Decision
Default `SessionConfig::max_streams = 16`, matching the spec default maximum. Configurable at construction time. Validation happens before any per-stream resource allocation (no partial allocation).

### Tradeoffs
Setting exactly 16 as the default gives maximum spec compliance. Operators wanting more streams must explicitly configure a higher limit.

### Spec Reference
Section 5.6

## Phase 2: Buffer window limits
**Date:** 2026-05-28
**Phase:** Phase 2 — Session Layer
**Status:** Decided

### Context
The spec §7.4.1 says the default maximum buffer window MUST NOT exceed 30,000 ms. Requests exceeding the maximum must be clamped.

### Decision
`SessionConfig::max_buffer_window_ms = 30,000`. Client-requested values are clamped to this maximum before being reflected in `culpeo.init-ack`. The `SessionConfig::default_buffer_window_ms = 5,000` is used when the client omits `Buffer-Window`.

### Tradeoffs
The 30-second maximum matches the spec requirement. A 5-second default is conservative but avoids large buffer allocations for applications that don't implement resumption.

### Spec Reference
Section 7.4, 7.4.1

## Phase 2: PCM offset overflow protection
**Date:** 2026-05-28
**Phase:** Phase 2 — Session Layer
**Status:** Decided

### Context
The PCM offset calculation `frame_bytes / (channels × bits/8)` involves two intermediate multiplications and a division that could theoretically overflow. The agent spec requires integer overflow protection.

### Decision
All intermediate values are promoted to `uint64_t` before arithmetic. The denominator `channels × (bits/8)` is computed as `uint64_t` to prevent uint16 overflow. The final addition `stream.offset + increment` is checked against `UINT64_MAX` before being applied. Zero channels or zero bits return `Error::offset_overflow` immediately.

### Tradeoffs
The overflow guard on `stream.offset + increment` is theoretically only reachable after ~584,542 years at 1M samples/sec, but the guard costs one comparison and eliminates undefined behavior regardless of workload.

### Spec Reference
Section 8.2, agent instructions Security Requirements

## Phase 2: JSON body parsing with nlohmann/json
**Date:** 2026-05-28
**Phase:** Phase 2 — Session Layer
**Status:** Decided

### Context
The session layer must parse JSON bodies for `culpeo.init`, `culpeo.ping`, `culpeo.pong`, `culpeo.auth-refresh`, and `culpeo.auth-response`. Options: write a custom parser, use nlohmann/json, use simdjson.

### Decision
Use nlohmann/json v3.11.3 fetched via CMake `FetchContent` (header-only mode). All JSON operations are wrapped in `try/catch` blocks that convert exceptions to `Error::json_error`. The public API remains `std::expected`-based throughout; nlohmann exceptions never escape the session layer.

For outgoing JSON frames (serialized responses), simple string formatting is used instead of nlohmann for constant-structure bodies (`pong`, `auth-refresh`), avoiding per-frame JSON object construction overhead.

### Tradeoffs
nlohmann/json adds ~3 seconds of first-build compile time. In exchange, the parser handles all valid JSON edge cases correctly (escaped strings, Unicode, large integers) without custom implementation risk. Since JSON parsing only happens on `culpeo.init` (rare) and auth events (rare), the per-call cost is irrelevant.

### Spec Reference
Section 6.1 (all control frame bodies)

## Phase 2: Session ID entropy
**Date:** 2026-05-28
**Phase:** Phase 2 — Session Layer
**Status:** Decided

### Context
The spec §A.5 requires session IDs to be at least 128 bits of entropy from a CSPRNG.

### Decision
Session IDs: 16 bytes from `RAND_bytes` → hex-encoded to 32 characters (128 bits of entropy). Stream IDs: 8 bytes from `RAND_bytes` → hex-encoded to 16 characters (64 bits of entropy). Stream IDs only need uniqueness within a session (max 16 streams), so 64 bits provides negligible collision probability (birthday bound: ~4.3×10^-18 for 16 IDs).

### Tradeoffs
Session IDs exactly meet the 128-bit requirement. Stream IDs use less entropy than session IDs since their security requirement is only within-session uniqueness, not global unguessability.

### Spec Reference
Section 5.3, Section A.5, agent instructions Security Requirements

## Phase 2: Pimpl idiom for Session
**Date:** 2026-05-28
**Phase:** Phase 2 — Session Layer
**Status:** Decided

### Context
The session implementation includes nlohmann/json and OpenSSL headers. Exposing these in the public header would force all consumers to depend on them at compile time.

### Decision
`Session` uses the Pimpl idiom (`std::unique_ptr<Impl>`). All implementation details, internal helpers, and dependencies are in `session.cpp`. The public header only depends on `culpeo/message.hpp` and the C++ standard library.

### Tradeoffs
One extra heap allocation per session for the Impl struct. This is negligible compared to session-level overhead. In exchange, the public header is clean and ABI-stable: changing implementation details (e.g., upgrading JSON library version) doesn't require recompiling consumers.

### Spec Reference
C++ agent instructions Technical Requirements

## Security fix: max_header_count added to ParseLimits
**Date:** 2026-05-28
**Phase:** Phase 1 — Frame Layer (security fix)
**Status:** Decided

### Context
The Security Agent Phase 1 review (finding: cpp-missing-header-count-limit, severity High) identified that `ParseLimits` had no `max_header_count` field, allowing an attacker to send a frame with an unbounded number of headers and exhaust server memory despite the header block size limit. This was a spec §4.1.1 violation.

### Decision
Added `max_header_count{64}` to `ParseLimits` and enforce it at the top of each iteration in the `parse_headers` loop, returning `header_block_too_large` when exceeded.

### Tradeoffs
64 headers is well above any legitimate protocol usage (CulpeoStream control frames use fewer than 10 reserved headers). The error code reuses `header_block_too_large` because the spec does not define a dedicated header count error; this is consistent with "block rejected before parsing is complete".

### Spec Reference
Section 4.1.1 (parser limits)

## Security fix: NUL byte rejection in header values
**Date:** 2026-05-28
**Phase:** Phase 1 — Frame Layer (security fix)
**Status:** Decided

### Context
The Security Agent Phase 1 review (finding: cpp-null-byte-in-header-values, severity Medium) identified that `valid_header_value` only rejected `\r` and `\n` but not NUL bytes. Spec §4.1 requires rejection of CR, LF, and NUL in both header names and values.

### Decision
Added `ch == '\0'` to the rejection check in `valid_header_value`. NUL bytes in header names were already rejected via `valid_header_name`.

### Tradeoffs
No tradeoffs — NUL bytes are never valid in header values per the spec.

### Spec Reference
Section 4.1

## Fuzzer corpus seeded with required adversarial inputs
**Date:** 2026-05-28
**Phase:** Phase 1 — Frame Layer (security fix)
**Status:** Decided

### Context
The Security Agent Phase 1 review (finding: cpp-no-fuzzer-corpus, severity Medium) identified that the fuzzer had no corpus directory. LibFuzzer with no corpus starts from scratch and takes much longer to find interesting paths.

### Decision
Created `libculpeo-message/fuzz/corpus/` with 10 seed files covering all required adversarial inputs: valid frame, truncated (no terminator), CRLF injection in value, overlength block, NUL in name, NUL in value, binary with no terminator, too many headers (>64), empty frame, and only-terminator.

### Tradeoffs
Seed files make the fuzzer immediately effective at the boundaries the Security Agent identified. The corpus should be expanded over time as the fuzzer finds new interesting inputs.

### Spec Reference
Section 4.1, Section 4.1.1; C++ agent instructions (fuzz corpus requirements)

## Remove OpenSSL dependency — use std::random_device with platform allowlist
**Date:** 2026-05-28
**Phase:** Phase 2 — Session Layer
**Status:** Decided
**Supersedes:** "Phase 2: Nonce generation approach", "Phase 2: Nonce comparison timing safety"

### Context
The session layer previously depended on OpenSSL (`libcrypto`) for three primitives: `RAND_bytes` (CSPRNG), `OPENSSL_cleanse` (secure zeroing), and `CRYPTO_memcmp` (constant-time comparison). OpenSSL is a heavy dependency (~2MB shared lib) pulled in for three small functions, complicates cross-compilation, and blocks potential WASM compilation of the session layer.

Options considered:
1. Keep OpenSSL (status quo)
2. Direct OS CSPRNG APIs (`getrandom`, `arc4random_buf`, `BCryptGenRandom`) per platform
3. `std::random_device` with a compile-time platform allowlist

### Decision
Introduced `libculpeo-session/src/crypto.hpp`, a thin internal wrapper providing three functions:

**`secure_random(std::span<std::byte>)`** — uses `std::random_device` on platforms where the backing is a known CSPRNG (Linux/libstdc++ → `/dev/urandom`, macOS/libc++ → `/dev/urandom`, Windows/MSVC → `BCryptGenRandom`). A compile-time `static_assert` rejects unverified platforms. Emscripten is handled via `emscripten_get_entropy()`. The `random_device` instance is `thread_local` to avoid construction overhead per call.

**`secure_zero(void*, size_t)`** — uses `explicit_bzero` on glibc/macOS, `SecureZeroMemory` on Windows, with a `volatile unsigned char*` loop fallback for other platforms (e.g., musl, older Android NDK).

**`constant_time_equal(span<const uint8_t>, span<const uint8_t>)`** — XOR-accumulator over the full span length, returns false immediately on length mismatch (length is not secret). No content-dependent branches.

### Tradeoffs
- **Gained:** Eliminated the OpenSSL build dependency entirely from `libculpeo-session`. Simpler cross-compilation (Android NDK, iOS). Emscripten path available for future WASM builds. Single API across platforms instead of three OS-specific branches.
- **Gave up:** Direct kernel API calls — we trust the toolchain's `std::random_device` mapping instead. This is well-documented and stable on the allowlisted platforms but is one layer of indirection further from the entropy source.
- **Risk accepted:** The `volatile` fallback for `secure_zero` on unknown platforms is less formally guaranteed than `explicit_bzero`, but the `static_assert` on the random path ensures we only reach it on platforms we've consciously added.
- **Risk accepted:** The XOR-accumulator for constant-time comparison is standard practice but lacks the formal audit pedigree of `CRYPTO_memcmp`. For 32-byte nonce buffers this is well-understood and sufficient.

### Spec Reference
Section A.4, A.5, agent instructions Security Requirements

## OffsetType replaces StreamCodec for offset increment behaviour
**Date:** 2026-05-27
**Phase:** Phase 2 — Session Layer
**Status:** Decided

### Context
The original offset_tracker used `StreamCodec` (inferred from content_type) to determine how a stream's offset advances after each media frame:
- `StreamCodec::pcm` → PCM sample-count formula
- Everything else → increment by 1

The spec §5.5 was updated to require an explicit `offset_type` field on every stream declaration. Three values are defined: `time` (PCM formula), `byte` (raw byte length of the payload), and `message` (always 1). The `byte` type is entirely new — no codec inference could have produced it. Missing or unrecognised values must be rejected as `invalid-streams` (spec §5.6 rule 4).

Options considered:
1. Keep `StreamCodec` inference as the default and allow `offset_type` to override it
2. Make `offset_type` mandatory and drive all offset logic from it, keeping `StreamCodec` only for content-type introspection (PCM param extraction)
3. Remove `StreamCodec` entirely

### Decision
Option 2. `OffsetType` is now the sole driver of offset increment behaviour in `advance_offset`. `StreamCodec` is retained on `StreamInfo` for a different purpose: identifying whether to populate `pcm_params` (needed by `OffsetType::time`). The `advance_offset` implementation switches on `stream.offset_type` exclusively:
- `time`    → `compute_pcm_increment(frame_bytes, *stream.pcm_params)`
- `byte`    → `increment = frame_bytes`
- `message` → `increment = 1`

`OffsetType` is declared as `std::optional<OffsetType>` in `StreamDeclaration` (absent = not provided in JSON) and as a non-optional `OffsetType` in `StreamInfo` (always set once validated). The JSON parsing in `handle_init` rejects missing or unrecognised `offset_type` strings before they ever reach `validate_declarations`, and `validate_declarations` also guards against the `std::nullopt` case should a caller bypass JSON parsing.

### Tradeoffs
- Removing `StreamCodec` as the source of truth for offset behaviour eliminates an implicit coupling between MIME type and offset semantics. Streams can now declare `byte` offsets on Opus payloads (valid for some transport use cases) or `message` offsets on PCM streams.
- The `StreamCodec` field is now slightly redundant for the common case but still useful for content-type introspection and for code paths that need to know whether PCM params are applicable. A future cleanup could remove it if those paths migrate to checking `pcm_params.has_value()` directly.
- The `byte` offset type interacts with the overflow guard in `advance_offset` in the same way as `time` offsets: the guard is `increment > UINT64_MAX - stream.offset`. For `byte`, `increment = frame_bytes` which can legally be 0 (no-op advance) or very large. A single frame of `UINT64_MAX - current_offset + 1` bytes would overflow; the guard returns `Error::offset_overflow` and closes the session, consistent with the behaviour for PCM overflow.
- Zero-byte payloads with `offset_type=byte` produce zero increment (no advance), which is correct per spec §8.2: "the increment applied after each media frame is delivered".

### Spec Reference
Section 5.5, 5.6 (rule 4), 8.2
