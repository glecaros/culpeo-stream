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

## Content-Type comparison: case-insensitive for names, case-sensitive for values (CPP-001)
**Date:** 2026-05-30
**Phase:** Phase 2 — Session Layer
**Status:** Decided

### Context
The original Content-Type comparison in `process_media_frame` lowercased the entire string before comparing, making parameter values case-insensitive. Spec §6.2 requires type/subtype and parameter *names* to be case-insensitive but parameter *values* to be case-sensitive.

### Decision
Implemented `content_type_matches(declared, received)` in the anonymous namespace of `session.cpp`. It:
1. Splits each string at `;` into a type/subtype part and parameter key=value pairs.
2. Compares type/subtype case-insensitively via `iequals_sv`.
3. Builds two parameter arrays (capped at 16 entries each).
4. For each declared parameter, finds a matching received parameter with case-insensitive name and case-sensitive value comparison.
5. Requires equal parameter counts (neither side may have extra parameters).

The function is allocation-free (uses `std::string_view` throughout and a fixed-size `std::array<CtParam, 16>`).

### Tradeoffs
- A cap of 16 parameters per Content-Type is more than sufficient for any audio/pcm or codec type, but would fail (return false) for pathological inputs with >16 params. This is acceptable behaviour — such inputs are already invalid per spec §6.2.
- Parameter count equality is enforced: if the frame carries extra parameters not declared on the stream, it is rejected. This is the strictest possible interpretation of "must match stream declaration".

### Spec Reference
Section 6.2

## Mandatory Content-Type on media frames (SEC-014)
**Date:** 2026-05-30
**Phase:** Phase 2 — Session Layer
**Status:** Decided

### Context
The previous code only validated Content-Type when it was present, silently accepting media frames with no Content-Type header. Per spec §6.2 and the security review (SEC-014), Content-Type is mandatory on media frames.

### Decision
`process_media_frame` now returns `Error::protocol_error` and closes the session with `"protocol-error"` if `f.content_type` is absent. The check is placed immediately before the content-type match check (after offset validation succeeds).

### Tradeoffs
Any client implementation that omitted Content-Type on media frames will now be rejected. The spec is clear that Content-Type is required; this is a correctness fix.

### Spec Reference
Section 6.2, Security review SEC-014

## Remove dead offset arithmetic (CPP-003)
**Date:** 2026-05-30
**Phase:** Phase 2 — Session Layer
**Status:** Decided

### Context
In `process_media_frame`, lines 999–1002 performed the no-op `stream_snapshot.offset -= (stream->offset - stream_snapshot.offset)` then immediately re-assigned `stream_snapshot = *stream`. Both snapshots were identical (since the subtraction was zero), and the comment was misleading.

### Decision
Removed the dead line. The single `StreamInfo stream_snapshot = *stream` after `advance_offset` correctly captures the post-advance state.

### Tradeoffs
Pure cleanup — no behaviour change.

## Replace std::random_device with OS CSPRNG primitives (SEC-013)
**Date:** 2026-05-30
**Phase:** Phase 2 — Session Layer
**Status:** Decided

### Context
`crypto.hpp` used `thread_local std::random_device` as the entropy source. While libstdc++ on Linux backs `std::random_device` with `/dev/urandom`, this is an implementation detail — the standard does not guarantee it. The Security Agent (SEC-013) required explicit OS CSPRNG calls.

### Decision
Replaced the `std::random_device` fill loop with platform-specific CSPRNG calls:
- **Linux/Android**: `getrandom(2)` via `<sys/random.h>` — blocks until entropy pool is seeded, then uses the ChaCha20-based kernel CSPRNG. Never uses a file descriptor. Retries on `EINTR`.
- **macOS/iOS**: `arc4random_buf(3)` — ChaCha20-based, never fails, never returns partial data.
- **Windows**: `BCryptGenRandom(NULL, ..., BCRYPT_USE_SYSTEM_PREFERRED_RNG)` — uses the system-preferred RNG without requiring an algorithm handle.
- **Emscripten**: unchanged — `emscripten_get_entropy` delegates to `crypto.getRandomValues`.

Removed `#include <random>` since `std::random_device` is no longer used.

### Tradeoffs
- `getrandom` on very early Linux (< 3.17) is absent; the code will fail to compile on those kernels. This is intentional — pre-3.17 kernels are end-of-life and lack modern security properties.
- `arc4random_buf` was already available on macOS 10.7+ and all iOS versions, so no compatibility regression.

### Spec Reference
Agent instructions — Security Requirements (CSPRNG selection)

## Cryptographically random ping nonce instead of timestamp (SEC-016)
**Date:** 2026-05-30
**Phase:** Phase 2 — Session Layer
**Status:** Decided

### Context
`send_ping` used the Unix epoch microsecond timestamp as both the ping identifier and the RTT anchor. A predictable identifier is a security weakness — an attacker who can observe or influence the timestamp could forge a pong response. The Security Agent (SEC-016) required a CSPRNG-backed nonce.

### Decision
1. `pending_ping_ts` replaced by two fields in `Impl`:
   - `pending_ping_nonce` (`std::optional<uint64_t>`) — CSPRNG-generated 64-bit nonce.
   - `pending_ping_send_ts` (`std::optional<int64_t>`) — microsecond timestamp at send time (for RTT computation).
2. `generate_u64_nonce()` helper fills a `uint64_t` via `culpeo::crypto::secure_random`; re-rolls on the astronomically unlikely all-zero result.
3. Ping body changed to `{"nonce": <uint64>, "server_ts": <microseconds>}`.
4. `handle_pong` reads `"nonce"` from the pong body (instead of `"ts"`), validates it against `pending_ping_nonce`, and computes RTT from `now_us() - pending_ping_send_ts`.
5. The test `"Server-initiated ping → RTT callback on pong"` was updated to extract `nonce` from the ping body and echo it in the pong.

### Tradeoffs
- Pong body field change from `"ts"` → `"nonce"` is a breaking change for clients that only look at the `"ts"` field in server-initiated pongs. The spec did not prescribe a format for server-initiated ping bodies; this aligns with the new spec wording.
- RTT is now computed entirely server-side from a stored send timestamp, not from a value echoed by the client. This removes the possibility of a client manipulating the RTT measurement by altering the echoed timestamp.

### Spec Reference
Agent instructions — Security Requirements (CSPRNG), Security review SEC-016

## PCM parameter semantic validation: channels≥1 and bits%8==0 (CPP-002)
**Date:** 2026-05-30
**Phase:** Phase 1 — Frame Layer
**Status:** Decided

### Context
The PCM content-type parser accepted `channels=0` (no channels) and `bits=7` (non-byte-aligned samples), which are nonsensical values. The Security Agent / code review (CPP-002) required these to be rejected.

### Decision
After successfully parsing all three PCM parameters, two additional guards are applied:
```cpp
if (*channels == 0) return std::unexpected(Error::invalid_content_type);
if (*bits == 0 || *bits % 8 != 0) return std::unexpected(Error::invalid_content_type);
```
`parse_u32` also now rejects leading zeros (e.g. `"016000"` is not a valid decimal integer per spec §4.2). This was added as a single guard:
```cpp
if (value.size() > 1 && value[0] == '0') return std::unexpected(Error::invalid_content_type);
```

### Tradeoffs
- `channels=0` and non-byte-aligned `bits` values are spec violations (there is no meaningful PCM stream with zero channels). Rejecting them at parse time prevents downstream arithmetic errors in `compute_pcm_increment`.
- Leading-zero rejection aligns `parse_u32` with `parse_uint64` (which already rejected them) for consistency.

### Spec Reference
Section 6.1 (audio/pcm parameter semantics), CPP-002 review finding

## Phase 3: WebSocket library choice — uWebSockets over libwebsockets
**Date:** 2026-05-31
**Phase:** Phase 3 — Transport Adapter
**Status:** Decided

### Context
Phase 3 requires a concrete WebSocket transport adapter.  Two candidates were evaluated:

**uWebSockets v20 (µWS)**
- License: Apache 2.0 — permissive, compatible with commercial use without binary linkage obligations.
- API: Pure C++17/20 header-only library backed by uSockets (C event loop).  Typed generics (`uWS::WebSocket<SSL, isServer>*`) fit naturally with C++ session layers.
- Performance: Routinely benchmarked at the top of WebSocket server benchmarks (>1M msg/s on commodity hardware).
- TLS: Handled at the App level (`uWS::SSLApp` vs `uWS::App`); per-connection handles are TLS-transparent.
- Dependency footprint: uWebSockets headers (C++) + uSockets (C, ~5 source files).
- Drawback: Not thread-safe — all operations on a `WebSocket*` must happen on the owning event-loop thread.

**libwebsockets**
- License: LGPL 2.1 (with exceptions) — requires careful handling in proprietary products; static linking triggers stronger copy-left obligations.
- API: C API with callback-driven dispatch; C++ wrappers are unofficial.
- TLS: Well-supported via OpenSSL, mbedTLS, wolfSSL.
- Dependency footprint: Heavier — full C library with many optional protocol plugins.
- Drawback: C API is significantly more verbose to integrate from a C++ session layer; error handling requires checking return codes on every lws function.

### Decision
**uWebSockets**.  Primary reasons:
1. **License** — Apache 2.0 avoids LGPL linkage obligations.
2. **C++ API** — generic WebSocket handle integrates cleanly with C++ callback patterns; the `uws_adapter.hpp` factory is 30 lines.
3. **Performance** — consistent with the Phase 1 (<100 ns/frame) design target; uWS adds negligible overhead per send.
4. **Header-only C++ layer** — the C++ headers are included as an INTERFACE CMake target (no extra compilation); only uSockets needs compiling.

### Tradeoffs
- The event-loop thread-affinity constraint requires off-loop sends to be deferred via `uWS::Loop::get()->defer()`.  `uws_adapter.hpp` handles this transparently.
- uSockets must be built separately and linked into the final binary.  The `culpeo_transport_ws` library itself does NOT link against uSockets, keeping the core transport library lean and dependency-free.  Users who want the uWS backend include `<culpeo/uws_adapter.hpp>` and link uSockets themselves.

### Spec Reference
Addendum B — WebSocket Binding; C++ agent instructions Phase 3

## Phase 3: WsTransport design — callback injection over direct WebSocket coupling
**Date:** 2026-05-31
**Phase:** Phase 3 — Transport Adapter
**Status:** Decided

### Context
`ITransport` is an abstract interface.  The concrete `WsTransport` implementation must:
1. Work with any WebSocket library (not just uWebSockets).
2. Be unit-testable without a running WebSocket server.
3. Be thread-safe for concurrent sends (session layer may call `send_media` from worker threads).

Options considered:
1. Template the transport on the WebSocket type (`WsTransport<uWS::WebSocket<SSL, true>>`)
2. Erase the WebSocket handle behind another abstract interface (`IWsConnection`)
3. Inject `std::function` callbacks for send_text, send_binary, and close

### Decision
Option 3 — `std::function` callback injection.

```cpp
class WsTransport : public ITransport {
    std::function<void(std::span<const std::byte>)> send_text_fn_;
    std::function<void(std::span<const std::byte>)> send_binary_fn_;
    std::function<void(int, std::string_view)>      close_fn_;
    std::mutex mu_;  // serializes all three operations
};
```

A separate `uws_adapter.hpp` header provides a `make_uws_transport<SSL>(ws)` factory that creates the correct lambdas for a uWebSockets handle.  Tests pass mock lambdas with zero boilerplate.

### Tradeoffs
- `std::function` has a small indirect call overhead vs a virtual dispatch through a second interface.  For WebSocket sends (dominated by syscall cost), this is entirely negligible.
- The callback approach means `WsTransport` cannot introspect the underlying connection state (e.g., is the socket still open?).  For the current API surface this is not needed; the session layer tracks its own state.
- `std::function` copies of spans are not made automatically — the buffer passed to the callback is only valid for the call duration.  `uws_adapter.hpp` copies into a `std::vector<std::byte>` before deferring to the event loop; tests that inspect data synchronously are safe.

### Spec Reference
C++ agent instructions Phase 3 — ITransport interface, thread-safety requirements

## Phase 3: ITransport::close() signature — integer WebSocket code + reason
**Date:** 2026-05-31
**Phase:** Phase 3 — Transport Adapter
**Status:** Decided

### Context
The original `ITransport::close()` carried no arguments.  For a WebSocket transport, the close frame must carry a numeric status code (RFC 6455 §7.4.1) and an optional reason string.  Adding these makes the transport responsible for sending a well-formed WebSocket close frame rather than relying on the library's default.

CulpeoStream uses string close codes ("normal", "protocol-error", "unauthorized", etc.) internally; these must be mapped to WebSocket integer codes.

### Decision
Updated `ITransport::close()` to:
```cpp
virtual void close(int code, std::string_view reason) = 0;
```

Added `to_ws_close_code(std::string_view culpeo_code)` as a private static helper in `Session::Impl`, and a `close_transport(culpeo_code, reason)` wrapper that calls it.  All 18 call sites in `session.cpp` were updated.

Mapping:
- `"normal"` → 1000 (Normal Closure)
- `"unauthorized"` / `"auth-expired"` / `"auth-failed"` → 1008 (Policy Violation)
- All other codes (`"protocol-error"`, `"version-unsupported"`, etc.) → 1002 (Protocol Error)

The reason string passed to the transport is the same human-readable reason already present in the corresponding `culpeo.close` or `culpeo.init-error` protocol frame.

### Tradeoffs
Existing users of `ITransport` must update their `close()` override (one-line change: add `int code, std::string_view reason` parameters).  The `MockTransport` in the test suite was updated accordingly and now also captures `close_code` and `close_reason` for assertions.

### Spec Reference
RFC 6455 §7.4.1; Addendum B §B.3; C++ agent instructions Phase 3

## Atomics-first concurrency policy
**Date:** 2026-05-31
**Phase:** Cross-cutting
**Status:** Decided

### Context
Phase 3 introduced concurrency requirements in the transport layer (WsTransport mutex, uws_adapter shutdown flag). There was a tendency to use `std::mutex` broadly, even for simple boolean shutdown guards where a `std::atomic<bool>` suffices.

### Decision
Prefer `std::atomic<T>` for state flags, shutdown guards, and "already initialized" checks. Only use `std::mutex` when holding it across compound operations that must be uninterrupted (e.g. serializing a frame and appending it to a send queue atomically). Document the reason for each mutex at the declaration site.

### Tradeoffs
Atomics are cheaper (no kernel involvement, no contention sleep), but cannot atomically update two fields together. For single-field guards or flags, atomics are strictly superior. Mutexes remain appropriate for protecting multi-field invariants.

### Spec Reference
N/A (implementation policy)

## Phase 3 review fixes: alive flag, exception swallowing, close-code mapping, reason sanitization
**Date:** 2026-05-31
**Phase:** Phase 3 — Transport Adapter
**Status:** Decided

### Context
Phase 3 review (CPP-P3-001 through CPP-P3-004, SEC-017) identified five issues in `libculpeo-transport-ws`:

1. **CPP-P3-001** — All `loop->defer()` lambdas in `uws_adapter.hpp` captured raw `uWS::Loop*` and `uWS::WebSocket*` without guarding against the socket/loop being destroyed before the deferred callback fired (use-after-free / UB on shutdown).
2. **CPP-P3-002** — `std::function` callbacks invoked in `send_text`, `send_binary`, `close` could throw, causing an unhandled exception to propagate out of `ITransport` method calls, potentially crashing the event-loop thread.
3. **CPP-P3-003** — `to_ws_close_code()` mapped `"server-shutdown"` and `"idle-timeout"` to RFC 6455 code 1002 (Protocol Error) rather than 1001 (Going Away), which misrepresents the close reason to the peer.
4. **CPP-P3-004** — `Session` constructor and `uws_adapter.hpp` usage comment did not document the requirement that `ITransport` must outlive the `Session` instance, causing subtle lifetime bugs if the teardown order was wrong.
5. **SEC-017** — `WsTransport::close()` passed the reason string directly to the callback without enforcing RFC 6455 §5.5.1 constraints: reason ≤ 123 bytes and no ASCII control characters.

### Decision

**CPP-P3-001 (`uws_adapter.hpp`):**
`make_uws_transport()` now creates a `shared_ptr<atomic<bool>> alive` flag (initialized `true`) and captures it in every lambda. Each lambda guards with `alive->load(acquire)` before calling `loop->defer()`, and the inner defer lambda guards again before touching `ws`. The flag is returned to the caller as `UwsTransportResult::alive`. The `.close` handler MUST store it and call `alive->store(false, release)` at the very start, BEFORE resetting session or transport. The usage example in the file header was updated with the complete, correct shutdown sequence.

**CPP-P3-002 (`transport_ws.cpp`):**
All three `WsTransport` methods now wrap their callback invocations in `try { ... } catch (...) {}`. Transport-level exceptions are non-fatal: logging would require a logger dependency; silencing is safe because the session layer already has a best-effort close contract.

**CPP-P3-003 (`session.cpp` `to_ws_close_code`):**
Added a new branch before the catch-all `return 1002`:
```cpp
if (culpeo_code == "server-shutdown" || culpeo_code == "idle-timeout") return 1001;
```

**CPP-P3-004 (`session.hpp`, `uws_adapter.hpp`):**
Added a Doxygen `@param transport` comment to the `Session` constructor explaining the lifetime requirement. Updated `uws_adapter.hpp` usage example to document the required Session-before-Transport teardown order.

**SEC-017 (`transport_ws.cpp` `WsTransport::close`):**
Before invoking the close callback:
1. Truncate reason to 123 bytes if longer (RFC 6455 §5.5.1: max payload 125 bytes minus 2-byte status code).
2. Replace all bytes with `unsigned char < 0x20` (except `\t`) with `'?'` to neutralise `\r\n` injection and other control characters.
Full UTF-8 validation was not added in this pass (the RFC does not prohibit multi-byte sequences with values ≥ 0x80; stripping control bytes is the minimum required fix). A follow-up can add a proper UTF-8 validator.

### Tradeoffs
- The `UwsTransportResult` struct is a breaking API change vs. the original `std::unique_ptr<WsTransport>` return type, but no production callers exist yet and the change forces correct shutdown discipline on all future users.
- Swallowing all exceptions in send/close loses observability but avoids crashing the event-loop thread; higher-level health monitoring (e.g. session on_error callback) is the correct place to surface errors.
- Truncating the close reason at 123 bytes is lossy but safe; long reasons are usually diagnostic text where the first 123 bytes retain context.

### Spec Reference
RFC 6455 §5.5.1, §7.4.1; CPP-P3-001 through CPP-P3-004; SEC-017

## C++20 coroutines for transport interface — deferred to Phase 4
**Date:** 2026-05-31
**Phase:** Phase 4 — HTTP/2 Transport
**Status:** Deferred (decision required in Phase 4)

### Context
The current `ITransport` interface is synchronous and void-returning. `WsTransport` uses `mutex` + `loop->defer()` to marshal sends onto the uWS event loop, with `try/catch` swallowing callback exceptions. This works but has two structural weaknesses: no backpressure (callers cannot know when a send completes) and no error propagation from sends.

HTTP/2 is the natural point to evaluate coroutines: stream multiplexing, flow control, and HEADERS-before-DATA sequencing all map better to `co_await` chains than to nested callbacks.

### Options considered
1. **Stay callback-based** — extend the existing pattern to HTTP/2. Works, but callback graphs for H2 stream lifecycle become complex.
2. **C++20 coroutines with Asio** (`asio::awaitable<T>`) — mature executor, `FetchContent`-friendly, good nghttp2 integration examples.
3. **Minimal custom awaitables** — no new dependency, but significant boilerplate for a correct executor.

### Decision
Deferred. The WebSocket transport does not need to change. Phase 4 agent MUST evaluate options 2 and 3, pick one, and record the decision. If coroutines are adopted for `H2Transport`, introduce a parallel `IAsyncTransport` interface rather than modifying the existing `ITransport` — the WebSocket transport can opt in later.

### Tradeoffs
Coroutines eliminate the mutex/defer dance and give natural backpressure and error propagation. The cost is a scheduler dependency (Asio) and more complex mock patterns in tests. Callbacks are simpler to test but harder to reason about under complex async sequences (H2 flow control, GOAWAY, RST_STREAM).

### Spec Reference
Addendum C (HTTP/2 binding)
