## Finding: culpeo_serialize_frame — null body pointer with non-zero body_len causes undefined behaviour

**Severity:** High
**Target:** C++
**Phase:** Phase 5 — C API shim / WASM

### Description

`culpeo_serialize_frame` in `c_api.cpp` (lines 161–165) constructs a
`std::span<const std::byte>` directly from the caller-supplied `body` pointer
and `body_len` without first checking that `body != nullptr` when `body_len > 0`.

```cpp
const auto bspan = std::span<const std::byte>(
    reinterpret_cast<const std::byte *>(body), body_len);
```

The existing null guards check:
```cpp
if (out_buf == nullptr || out_cap == 0)   return -1;
if (header_count < 0)                     return -1;
if (header_count > 0 && (headers == nullptr || strings_buf == nullptr)) return -1;
```

There is **no** corresponding check `if (body_len > 0 && body == nullptr)`.
Constructing `std::span` from a null pointer with a non-zero count is undefined
behaviour under the C++ standard (the span preconditions require a valid range).
In practice, downstream code in `culpeo::message::serialize_frame_to_buffer` will
attempt to read from address 0, causing a segmentation fault or, on hardened
platforms, SIGBUS / access violation.

### Attack Scenario

1. Attacker controls a C-language caller of `culpeo_serialize_frame` (or
   manipulates a WASM import table to substitute a payload).
2. Caller passes `body = NULL`, `body_len = 16`.
3. `culpeo_serialize_frame` does not detect the invalid combination, proceeds
   to form a span, and passes it to `serialize_frame_to_buffer`.
4. The serializer iterates over the body span, dereferencing NULL, triggering a
   crash in the host process.

In the primary WASM deployment, the TypeScript loader guards against this
(`body.length > 0 ? copyToHeap(...) : 0`), so the WASM path is safe today.
However, the C API contract has no such guarantee, and any other native consumer
(e.g., a future C# P/Invoke shim, a Python ctypes binding, or a C test harness)
is exposed.

### Impact

Process crash (denial of service). Depending on the memory layout, a platform
without ASLR or with a custom allocator that maps memory near address 0 could
potentially be exploited for memory disclosure or code execution, though this is
unlikely in practice.

### Suggested Fix

Add a null check before the body span construction:

```cpp
if (body_len > 0 && body == nullptr) {
    return -1;
}
const auto bspan = std::span<const std::byte>(
    reinterpret_cast<const std::byte *>(body ? body : reinterpret_cast<const uint8_t*>("")),
    body_len);
```

A cleaner form:

```cpp
if (body_len > 0 && body == nullptr) return -1;
const auto bspan = body != nullptr
    ? std::span<const std::byte>(reinterpret_cast<const std::byte *>(body), body_len)
    : std::span<const std::byte>{};
```

Add a documentation note to `c_api.h`: "When `body_len` is non-zero, `body`
must not be NULL; the function returns -1 otherwise."

### Spec Reference

N/A (C API implementation detail)
