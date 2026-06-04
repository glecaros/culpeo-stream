# `culpeo_serialize_frame` truncates `size_t` written-bytes to `int` without overflow guard

**Severity:** Low

**Location:** `implementations/cpp/libculpeo-message/src/c_api.cpp` line 173

## Description

`culpeo_serialize_frame` returns the number of bytes written as `int`, but the internal
`serialize_frame_to_buffer` function returns `size_t` (or an equivalent unsigned type).  The
conversion on the final return line:

```cpp
return static_cast<int>(*written);
```

is performed without any range check.  If `*written > INT_MAX` (2 147 483 647 on all
supported platforms), the result wraps to a negative value, which the caller interprets as an
error (`-1` indicates failure by contract).  The caller would silently discard a successfully
serialized, oversized frame and retry or abort unnecessarily.

While CulpeoStream frames in normal operation are far smaller than 2 GiB, the C API has no
documented frame-size cap, and the output buffer capacity (`out_cap`) is `size_t`.  A WASM
caller supplying a 2+ GiB buffer is technically within the API contract.

A related concern: the function signature returns `int`, but the argument `out_cap` is
`size_t`, creating an asymmetry where the caller must supply a `size_t` capacity but can only
receive an `int` byte count in return.

## Impact

- Silent success-reported-as-failure for frames larger than `INT_MAX` bytes (extremely
  unlikely in practice, but the API contract does not exclude it).
- The asymmetry between `out_cap: size_t` and return `int` makes the API slightly harder to
  use correctly from C/WASM (the caller must cast and check sign).

## Suggested Fix

Add an explicit overflow guard before returning:

```cpp
if (*written > static_cast<size_t>(INT_MAX)) {
    return -1;  // would truncate; caller must use a smaller buffer or split the frame
}
return static_cast<int>(*written);
```

Alternatively, change the return type to `int64_t` (which can represent any realistic frame
size) and update `c_api.h` accordingly.  This is a source-incompatible change for existing
callers but is the cleaner long-term fix.
