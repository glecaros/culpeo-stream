# `c_api.h` error code `-3` documented as count-only but also fired on byte-size overrun

**Severity:** Low

**Location:** `implementations/cpp/libculpeo-message/include/culpeo/c_api.h` lines 49–52

## Description

The documentation comment for `culpeo_parse_headers` documents the `-3` return code as:

```
-3     Header count exceeds max_headers or internal limit (64).
```

However, `-3` is also returned when `culpeo::message::Error::header_block_too_large` is
raised by the C++ parser (c_api.cpp line 52):

```cpp
case culpeo::message::Error::header_block_too_large:
    return -3; // header count / size limit exceeded
```

`header_block_too_large` is triggered by `ParseLimits::max_header_block_bytes` (default 8 192
bytes), not by header count.  A 9-header frame whose headers collectively exceed 8 192 bytes
returns `-3`, but the documented meaning says nothing about byte limits — a caller reading the
docs would assume the frame simply has too many headers and would try splitting it into smaller
batches, which will not help.

Additionally, the "internal limit (64)" claim describes `ParseLimits::max_header_count`
(default 64), but this is a **configurable** default, not a fixed constant.  A WASM consumer
that sets different parse limits on the C++ side will observe different limits without any way
to discover them through the C API.

## Impact

- WASM / FFI callers cannot distinguish "too many headers" from "header block too large in
  bytes" — both return `-3`.  This impedes correct error handling and user-facing diagnostics.
- Documentation-driven misdiagnosis: callers may reduce header count rather than header value
  sizes when the actual trigger is byte length.

## Suggested Fix

Either:

1. **Split into two distinct error codes** (`-3` for count, `-4` for byte size), or
2. **Update the documentation** to reflect both conditions precisely:

```c
 * @return  >= 0   Number of headers parsed.
 *          -1     \r\n\r\n terminator absent (incomplete frame — buffer more data).
 *          -2     Malformed header line (missing ':', invalid chars in name/value).
 *          -3     Header block too large: either the total byte size of the header
 *                 block exceeds the library's configured limit, or the number of
 *                 headers exceeds max_headers or the library's max_header_count.
```

Option 1 (split codes) is preferable for a C API consumed by JavaScript/WASM where the caller
has no access to C++ error types.
