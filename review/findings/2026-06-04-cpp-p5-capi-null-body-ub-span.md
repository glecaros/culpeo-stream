# `culpeo_serialize_frame`: null `body` with non-zero `body_len` constructs UB `std::span`

**Severity:** Medium

**Location:** `implementations/cpp/libculpeo-message/src/c_api.cpp` lines 130–173,
specifically lines 161–163

## Description

`culpeo_serialize_frame` validates several argument combinations but does not guard against
`body == nullptr` with `body_len > 0`:

```cpp
// Existing checks (lines 130–138):
if (out_buf == nullptr || out_cap == 0)          return -1;
if (header_count < 0)                            return -1;
if (header_count > 0 && (headers == nullptr || strings_buf == nullptr)) return -1;
// ↑ No check for body == nullptr && body_len > 0
```

The span is then constructed unconditionally (line 161):

```cpp
const auto bspan = std::span<const std::byte>(
    reinterpret_cast<const std::byte *>(body), body_len);
```

Constructing `std::span<T>(nullptr, N)` where `N > 0` violates the span precondition
(C++20 [span.cons] p15: "Requires: `[data, data + count)` is a valid range").  When
`serialize_frame_to_buffer` subsequently iterates over `bspan` to copy body bytes into the
output buffer, it dereferences the null pointer, producing undefined behaviour (typically a
crash under ASan or a silent write to address zero in release builds).

This is a C API that will be called from WebAssembly / JavaScript; the JavaScript caller may
legitimately pass a null `body` pointer (or a zero-length typed array whose `.byteOffset` is
coerced to 0 when the WASM allocator is not used) together with a computed non-zero length,
relying on the C function to return -1 gracefully.

## Impact

- Crash (SIGSEGV or ASan abort) when WASM / FFI caller passes `body=NULL, body_len>0`.
- UB in production: on platforms where a null-pointer `std::span` doesn't immediately fault,
  garbage data is read from address 0 and serialized into the output buffer, silently producing
  a malformed frame.

## Suggested Fix

Add an explicit null check immediately after the existing checks:

```cpp
if (body == nullptr && body_len > 0) {
    return -1;
}
```

Place it alongside the other argument-validation block (lines 130–138) so all error paths
are grouped together.
