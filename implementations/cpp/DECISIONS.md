## Zero-copy frame views
**Date:** 2026-05-26
**Phase:** Phase 1 — Frame Layer
**Status:** Decided

### Context
Phase 1 requires a parser that can inspect control and media frames without copying header values or the body. The transport layer already owns the receive buffer, so copying header strings into parser-owned storage would add avoidable latency and allocations.

### Decision
`libculpeo-frame` returns `ParsedHeadersView`, which stores `std::string_view` slices into the caller-owned frame buffer for the header block, reserved header values, and body. The public header documents this through the view-based API surface rather than owning strings.

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
The parser and content-type helpers return `std::expected<T, culpeo::frame::Error>`. `Error` is a compact enum that covers malformed header lines, invalid names or values, size-limit violations, duplicate reserved headers, invalid content types, and serialization buffer exhaustion.

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
Phase 1 ships `libculpeo-frame` as a normal compiled CMake target instead of a header-only library.

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
Split into a top-level `CMakeLists.txt` that sets project-wide settings and calls `add_subdirectory()`, and a per-library `libculpeo-frame/CMakeLists.txt` that owns its own targets, dependencies, and tests. Include paths use generator expressions (`$<BUILD_INTERFACE:...>`) to support both in-tree and installed usage.

### Tradeoffs
Adds one more CMake file per library, but each library is self-contained and can be built independently. New Phase 2+ libraries slot in with a single `add_subdirectory()` line.

### Spec Reference
C++ agent instructions Technical Requirements
