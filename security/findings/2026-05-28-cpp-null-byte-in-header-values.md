## Finding: C++ valid_header_value does not reject NUL bytes

**Severity:** Medium
**Target:** C++
**Phase:** Phase 1

### Description

The C++ `valid_header_value` function rejects CR (`\r`) and LF (`\n`) in header values but does **not** reject NUL bytes (`\0`):

```cpp
[[nodiscard]] constexpr bool valid_header_value(std::string_view value) noexcept {
    for (const char ch : value) {
        if (ch == '\r' || ch == '\n') {
            return false;
        }
    }
    return true;
}
```

By contrast, `valid_header_name` correctly rejects NUL (via the explicit `ch == '\0'` check and `is_token_char`). The C# implementation rejects `\r`, `\n`, **and** `\0` in both names and values via `ForbiddenHeaderBytes`. The TypeScript implementation rejects all three with `/[\r\n\0]/` applied to both fields. The spec (§4.1) states parsers MUST reject CR, LF, and NUL in both header names and header values.

### Attack Scenario

1. Attacker crafts a frame with a NUL byte embedded in a header value, e.g.:
   ```
   Reason: normal\x00 -- injected\r\n\r\n
   ```
2. The C++ parser accepts this frame and stores the value (including the NUL byte) in the `ParsedHeadersView.reason` `string_view`.
3. Downstream code that treats the `string_view` as a C-style string (e.g., via `.data()` passed to a logging function, `printf`, or a C API) will see only the bytes before `\0`, silently truncating the value.
4. In a log-injection context: if the logged reason contains `\0\nERROR: authentication succeeded for admin`, the log entry appears to show a benign truncated reason, while the malicious content is still in memory and may appear in raw log dumps or binary formats.
5. In a cross-implementation gateway scenario: a frame that C# or TypeScript would reject as malformed is accepted and forwarded by the C++ layer, bypassing the rejection rules of the downstream implementation.

### Impact

- **Cross-implementation divergence**: frames C# and TypeScript would reject pass through C++ unchallenged, violating spec §4.1.
- **Log injection / NUL truncation**: consumers of `string_view` values who pass `.data()` to C-string APIs receive silently truncated content.
- **Defense-in-depth violation**: NUL bytes in values are explicitly prohibited by the spec; the implementation's failure to enforce this leaves a footgun for any code that further processes the parsed value.

### Proposed Mitigation

Add NUL rejection to `valid_header_value`:

```cpp
[[nodiscard]] constexpr bool valid_header_value(std::string_view value) noexcept {
    for (const char ch : value) {
        if (ch == '\r' || ch == '\n' || ch == '\0') {
            return false;
        }
    }
    return true;
}
```

Add corresponding tests:
```
TEST_CASE("Reject null byte in header value", "[parser][error]") {
    // Frame with NUL inside a header value
    // Verify error == Error::invalid_header_value
}
```

Add a NUL-in-value seed to the fuzzer corpus.

### Spec Reference

§4.1 — Frame Header Format: "Implementations MUST reject any frame that contains CR, LF, or NUL (0x00) in a header name or value."
