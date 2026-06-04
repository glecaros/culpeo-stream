## Finding: culpeo_serialize_frame — return value narrowed from size_t to int

**Severity:** Low
**Target:** C++
**Phase:** Phase 5 — C API shim / WASM

### Description

`culpeo_serialize_frame` in `c_api.cpp` (line 173) casts the
`size_t`-typed number of bytes written to `int` before returning:

```cpp
return static_cast<int>(*written);
```

On LP64 platforms (Linux/macOS x86-64), `size_t` is 64 bits and `int` is 32
bits.  If `*written` exceeds `INT_MAX` (2,147,483,647 bytes ≈ 2 GiB), the cast
is implementation-defined and typically produces a negative value.  The caller
would then interpret the return as an error sentinel (any negative return means
failure), silently discarding a successful serialisation.

In the WASM deployment this is currently unexploitable because WASM32
`size_t` is 32 bits and `out_cap` is also 32 bits, making outputs > 2 GiB
impossible.  However, the C API is documented as usable from any C consumer, and
the function signature accepts `size_t out_cap` — implicitly allowing callers on
64-bit hosts to supply buffers larger than 2 GiB.

A more subtle risk: a future change to `out_cap` processing or to the
underlying serialiser that increases the output size could silently convert a
success into a reported failure for large frames, causing a caller to
double-free, skip a required output, or fall back to an insecure code path.

### Attack Scenario

1. A C native caller allocates a 3 GiB output buffer and calls
   `culpeo_serialize_frame` with `out_cap = 3 * 1024 * 1024 * 1024`.
2. The serialiser produces 2.2 GiB of output.  `*written = 2,365,587,456`.
3. `static_cast<int>(2365587456)` = `-1929379840` (implementation-defined
   signed overflow).
4. The caller sees a negative return value, treats it as error, and either
   retries (resource exhaustion) or abandons the output (logic error).

### Impact

Denial of service or silent logic failure for callers that produce very large
frames.  Not directly exploitable for memory corruption in the current WASM use
case, but represents a latent correctness-and-safety defect in the C API
contract.

### Suggested Fix

Change the return type of `culpeo_serialize_frame` to `ssize_t` (POSIX) or add
an output parameter for the byte count, using negative sentinels only for the
error path:

```c
/* Option A: use ssize_t (POSIX/glibc) */
ssize_t culpeo_serialize_frame(...);

/* Option B: separate error/count channels */
int culpeo_serialize_frame(..., size_t *bytes_written_out);
/* returns 0 on success (bytes_written_out set), -1 on error */
```

If `ssize_t` is not acceptable for portability, add a compile-time assertion
or a runtime guard:

```cpp
if (*written > static_cast<size_t>(INT_MAX)) {
    return -1; // frame too large for API contract
}
return static_cast<int>(*written);
```

Update `c_api.h` to document the maximum return value and the `INT_MAX`
constraint.

### Spec Reference

N/A (C API implementation detail)
