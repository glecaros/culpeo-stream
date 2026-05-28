## Finding: C++ ParseLimits missing max_header_count enforcement

**Severity:** High
**Target:** C++
**Phase:** Phase 1

### Description

The C++ `ParseLimits` struct exposes only two configurable limits:

```cpp
struct ParseLimits {
    std::size_t max_header_block_bytes{8192};
    std::size_t max_header_value_bytes{4096};
};
```

There is no `max_header_count` field, and the `parse_headers` function contains no per-header count check. The C# implementation enforces `MaxHeaderCount` (default 64) and the TypeScript implementation enforces `maxHeaderCount` (default 64), both as documented in the hardened spec §4.1.1 (`Maximum header count: 64`). The C++ implementation directly violates this spec requirement.

Within the 8 KiB block limit (8192 bytes), an attacker can send headers of the form `A: B\r\n` (7 bytes each) and fit approximately **1,170 individual headers**—18× the intended limit of 64. Each header triggers an O(n) linear scan through `kReservedHeaderNames` (10 entries) via `set_if_reserved`. Processing 1,170 headers burns roughly 18× more CPU than the spec-intended maximum.

### Attack Scenario

1. Attacker opens a WebSocket connection to a CulpeoStream endpoint backed by `libculpeo-frame`.
2. Attacker sends a frame whose header block is filled with unique short headers up to the 8 KiB block limit (e.g., 1,000–1,100 headers of 7–8 bytes each).
3. Every call to `parse_headers` on that connection processes all headers; each header triggers 10 string comparisons for reserved header matching.
4. Repeated rapidly, this provides a ~18× CPU amplification per request relative to what the spec permits.
5. For TLS-terminating proxies that pass the raw WebSocket frame to `libculpeo-frame`, the attacker pays TLS cost once but generates disproportionate parser CPU load on the server.

### Impact

CPU-based denial of service. Severity is elevated because:
- The spec explicitly mandates a 64-header maximum (§4.1.1).
- The other two implementations enforce it correctly, creating **cross-implementation behavioral divergence**: a frame that C# and TypeScript would reject as "too many headers" is silently processed by C++.
- Code that integrates all three libraries in a mixed deployment (e.g., C++ gateway + C# business logic) may route frames differently depending on which library parses them first, creating a security boundary inconsistency.

### Proposed Mitigation

1. Add `max_header_count` to `ParseLimits` with a default of `64`:
   ```cpp
   struct ParseLimits {
       std::size_t max_header_block_bytes{8192};
       std::size_t max_header_value_bytes{4096};
       std::size_t max_header_count{64};     // ADD THIS
   };
   ```
2. Add an `headerCount` counter in the `parse_headers` loop and return `Error::too_many_headers` (a new enum value) when the count exceeds the limit.
3. Add a corresponding unit test in `frame_tests.cpp`:
   ```
   TEST_CASE("Reject frame with more than max_header_count headers", "[parser][error]") {
       // Build a frame with 65 unique valid headers within the 8 KiB limit
       // Verify it fails with Error::too_many_headers
   }
   ```
4. Add a seed to the fuzzer corpus that contains exactly 65 minimal valid headers.

### Spec Reference

§4.1.1 — Parser Limits: "Maximum header count: 64 (REQUIRED)"
