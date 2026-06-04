## Finding: culpeo_serialize_frame — no strings_buf length bound → out-of-bounds read

**Severity:** High
**Target:** C++
**Phase:** Phase 5 — C API shim / WASM

### Description

`culpeo_serialize_frame` in `c_api.cpp` (lines 148–156) reads key and value
bytes from `strings_buf` using offsets stored in the caller-supplied
`culpeo_header` array, but there is **no `strings_buf_len` parameter** in the
function signature, and no bounds check is performed:

```cpp
for (int i = 0; i < header_count; ++i) {
    views[i].name = std::string_view(
        reinterpret_cast<const char *>(strings_buf + headers[i].key_ptr),
        headers[i].key_len);
    views[i].value = std::string_view(
        reinterpret_cast<const char *>(strings_buf + headers[i].val_ptr),
        headers[i].val_len);
}
```

If `headers[i].key_ptr + headers[i].key_len` or
`headers[i].val_ptr + headers[i].val_len` exceeds the actual size of the
`strings_buf` allocation, the code constructs a `std::string_view` that extends
beyond the allocation boundary. The downstream serializer then reads those bytes
as if they were valid header content.

In the WASM deployment, `strings_buf` is a WASM heap allocation.  A read beyond
the allocation boundary reads adjacent WASM heap memory — potentially other WASM
globals, the shadow stack, or previously freed data.  In native deployments the
read is a classic stack-or-heap buffer over-read.

### Attack Scenario

**Native C consumer path:**
1. Attacker injects crafted `culpeo_header` structs where `key_ptr = 0` and
   `key_len = 0xFFFFFFFF`.
2. `culpeo_serialize_frame` constructs a 4 GiB `string_view` starting at
   `strings_buf[0]`.
3. The serializer copies up to 4 GiB of process memory into the output buffer,
   subject to `out_cap`.  If `out_cap` is large, this leaks memory.

**WASM consumer path:**
1. A malicious script calling `serializeFrameWasm` directly (e.g., in a
   sandboxed context where user code can invoke WASM exports) cannot currently
   reach this path because `wasm-loader.ts` constructs the offsets itself.
2. However, if a future integration exposes raw WASM imports, or if a WASM heap
   corruption bug in another part of the module overwrites the header structs,
   the serializer becomes a gadget for reading arbitrary WASM heap memory and
   writing it into the output buffer.

### Impact

Memory disclosure (heap over-read).  In native deployments this can leak
cryptographic material, session tokens, or private key material that happens to
reside in the same heap.

### Suggested Fix

1. Add a `strings_buf_len` parameter to `culpeo_serialize_frame`:

```c
int culpeo_serialize_frame(const struct culpeo_header *headers,
                           int header_count,
                           const uint8_t *strings_buf,
                           size_t strings_buf_len,          /* NEW */
                           const uint8_t *body, size_t body_len,
                           uint8_t *out_buf, size_t out_cap);
```

2. Before constructing each `string_view`, validate the offset+length against
   `strings_buf_len`:

```cpp
for (int i = 0; i < header_count; ++i) {
    const uint32_t kEnd = headers[i].key_ptr + headers[i].key_len;
    const uint32_t vEnd = headers[i].val_ptr + headers[i].val_len;
    if (kEnd < headers[i].key_ptr || kEnd > strings_buf_len) return -1;
    if (vEnd < headers[i].val_ptr || vEnd > strings_buf_len) return -1;
    // ... construct views
}
```

3. Update `wasm-loader.ts` to pass `stringsTotal` as `strings_buf_len` in the
   WASM call.

### Spec Reference

N/A (C API implementation detail)
