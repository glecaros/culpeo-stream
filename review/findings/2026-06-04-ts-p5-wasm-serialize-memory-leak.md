# WASM heap memory leak in `serializeFrameWasm` when body `malloc` fails

**Severity:** High

**Location:** `implementations/typescript/packages/culpeostream-wasm/src/wasm-loader.ts`, lines 215–236

## Description

`serializeFrameWasm` allocates three WASM heap buffers sequentially without a unified cleanup guard:

```typescript
const stringsBufPtr = copyToHeap(mod, stringsBuf);                          // (1)
const bodyPtr = body.length > 0 ? copyToHeap(mod, body) : 0;               // (2)
const structsPtr = mod._malloc(headers.length * HEADER_STRUCT_SIZE || 1);  // (3)
if (structsPtr === 0) {
  mod._free(stringsBufPtr);
  if (bodyPtr !== 0) mod._free(bodyPtr);
  throw new Error(...);
}
```

`copyToHeap` throws a JavaScript `Error` when `_malloc` returns 0 (out-of-memory). If allocation (2) throws — because the WASM heap is full after allocation (1) already succeeded — then `stringsBufPtr` from (1) is permanently leaked: no code path frees it.

The subsequent manual `structsPtr === 0` guard does clean up both (1) and (2), but it is only reached when the `_malloc` call returns 0 without throwing. `copyToHeap` unconditionally throws on failure, bypassing that guard entirely.

The main `try/finally` block at lines 240–271 also does not cover this leak; it only wraps the code *after* all four allocations succeed.

## Impact

Each `serializeFrameWasm` call that hits an OOM on the body allocation leaks the strings buffer. With `ALLOW_MEMORY_GROWTH=1`, the WASM linear memory will grow on each call that approaches the current limit, and the leaked allocations prevent the freed slots from being reclaimed. Over time (e.g., in a long-running Node.js process serializing many frames under memory pressure) this can exhaust the process's virtual address space and crash the tab/process.

Under normal operation this path is rarely hit; under adversarial load (large header payloads combined with a memory-constrained environment) it becomes exploitable as a denial-of-service vector.

## Suggested Fix

Adopt the same `try/finally` pattern used in `parseHeadersWasm`: allocate all buffers before the `try`, and free all of them in the `finally`:

```typescript
const stringsBufPtr = copyToHeap(mod, stringsBuf);
// Wrap everything from here in a try/finally so stringsBufPtr is always freed.
let bodyPtr = 0;
let structsPtr = 0;
let outPtr = 0;
try {
  if (body.length > 0) bodyPtr = copyToHeap(mod, body);
  structsPtr = mod._malloc(headers.length * HEADER_STRUCT_SIZE || 1);
  if (structsPtr === 0) throw new Error('culpeostream-wasm: malloc failed for header structs');
  outPtr = mod._malloc(outCap || 2);
  if (outPtr === 0) throw new Error('culpeostream-wasm: malloc failed for output buffer');

  // ... write structs, call _culpeo_serialize_frame, slice result ...
} finally {
  if (outPtr !== 0) mod._free(outPtr);
  if (structsPtr !== 0) mod._free(structsPtr);
  if (bodyPtr !== 0) mod._free(bodyPtr);
  mod._free(stringsBufPtr);
}
```

This ensures `stringsBufPtr` (and every other allocation) is always freed, regardless of which allocation or operation throws.
