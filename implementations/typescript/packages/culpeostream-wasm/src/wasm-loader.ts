/**
 * wasm-loader.ts — Async loader for the Emscripten-compiled WASM module.
 *
 * Responsibilities:
 * - Import `createCulpeoParserModule` from `dist/culpeo_parser.js`
 * - Detect stub modules (Emscripten not yet run) and resolve `false`
 * - Wrap the low-level C functions in type-safe helpers
 * - Expose `parseHeadersWasm` / `serializeFrameWasm` that bridge between
 *   the C heap layout and JavaScript types
 *
 * Memory ownership
 * ----------------
 * Every WASM heap allocation is performed here and freed here before the
 * function returns.  No pointers escape.  Layout of `culpeo_header` struct:
 *
 *   offset  0: uint32_t  key_ptr
 *   offset  4: uint32_t  key_len
 *   offset  8: uint32_t  val_ptr
 *   offset 12: uint32_t  val_len
 *   sizeof   : 16 bytes
 */

import type { ParserBackend } from "culpeostream";
import type { CulpeoParserModule } from "../dist/culpeo_parser.js";

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

const HEADER_STRUCT_SIZE = 16; // sizeof(culpeo_header)
const MAX_HEADERS = 128; // generous upper bound
const UINT32_SIZE = 4;

let wasmModule: CulpeoParserModule | null = null;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

function requireModule(): CulpeoParserModule {
  if (wasmModule === null) {
    throw new Error("culpeostream-wasm: WASM module not initialised");
  }
  return wasmModule;
}

/**
 * Copy `src` bytes into the WASM heap and return the pointer.
 * Caller must free the returned pointer.
 */
function copyToHeap(mod: CulpeoParserModule, src: Uint8Array): number {
  const ptr = mod._malloc(src.length || 1); // malloc(0) is implementation-defined
  if (ptr === 0) {
    throw new Error("culpeostream-wasm: malloc failed");
  }
  mod.HEAPU8.set(src, ptr);
  return ptr;
}

/**
 * Read a uint32 from the WASM heap at `base + offset`.
 */
function readU32(
  mod: CulpeoParserModule,
  base: number,
  byteOffset: number,
): number {
  return mod.getValue(base + byteOffset, "i32") >>> 0;
}

// ---------------------------------------------------------------------------
// WASM-backed parse / serialize
// ---------------------------------------------------------------------------

/**
 * Parse a CulpeoStream header block using the WASM implementation.
 *
 * Returns an ordered list of [key, value] pairs and the body start offset,
 * or `null` if the `\r\n\r\n` terminator is absent.
 * Throws on malformed input.
 */
function parseHeadersWasm(buf: Uint8Array): {
  headers: ReadonlyArray<readonly [string, string]>;
  bodyOffset: number;
} | null {
  const mod = requireModule();

  // Allocate input buffer (read-only — the C++ parser does NOT mutate the
  // buffer; keys are returned in their original case)
  const inputPtr = copyToHeap(mod, buf);
  // Allocate headers_out array
  const headersPtr = mod._malloc(MAX_HEADERS * HEADER_STRUCT_SIZE);
  if (headersPtr === 0) {
    mod._free(inputPtr);
    throw new Error("culpeostream-wasm: malloc failed for headers array");
  }
  // Allocate body_offset_out (a single size_t = uint32 on wasm32)
  const bodyOffsetPtr = mod._malloc(UINT32_SIZE);
  if (bodyOffsetPtr === 0) {
    mod._free(headersPtr);
    mod._free(inputPtr);
    throw new Error("culpeostream-wasm: malloc failed for body offset");
  }

  let result: {
    headers: ReadonlyArray<readonly [string, string]>;
    bodyOffset: number;
  } | null = null;

  try {
    const count = mod._culpeo_parse_headers(
      inputPtr,
      buf.length,
      headersPtr,
      MAX_HEADERS,
      bodyOffsetPtr,
    );

    if (count === -1) {
      return null; // incomplete frame
    }
    if (count === -2) {
      throw new Error("culpeostream-wasm: malformed header line (missing ':')");
    }
    if (count === -3) {
      throw new Error("culpeostream-wasm: header count exceeds maximum");
    }
    if (count < 0) {
      throw new Error(
        `culpeostream-wasm: unexpected parse error code ${count}`,
      );
    }

    const bodyOffset = readU32(mod, bodyOffsetPtr, 0);
    const pairs: Array<readonly [string, string]> = [];

    for (let i = 0; i < count; i++) {
      const base = headersPtr + i * HEADER_STRUCT_SIZE;
      const keyPtr = readU32(mod, base, 0);
      const keyLen = readU32(mod, base, 4);
      const valPtr = readU32(mod, base, 8);
      const valLen = readU32(mod, base, 12);

      // Read bytes directly from the input buffer in heap.
      // We add inputPtr because key_ptr/val_ptr are offsets into the input.
      // The C++ parser does NOT lowercase keys; normalise case on the JS side.
      const keyBytes = mod.HEAPU8.slice(
        inputPtr + keyPtr,
        inputPtr + keyPtr + keyLen,
      );
      const valBytes = mod.HEAPU8.slice(
        inputPtr + valPtr,
        inputPtr + valPtr + valLen,
      );

      const key = new TextDecoder().decode(keyBytes).toLowerCase();
      const val = new TextDecoder().decode(valBytes);
      pairs.push([key, val] as const);
    }

    result = { headers: pairs, bodyOffset };
  } finally {
    mod._free(bodyOffsetPtr);
    mod._free(headersPtr);
    mod._free(inputPtr);
  }

  return result;
}

/**
 * Serialize header pairs and body using the WASM implementation.
 */
function serializeFrameWasm(
  headers: ReadonlyArray<readonly [string, string]>,
  body: Uint8Array,
): Uint8Array {
  const mod = requireModule();
  const encoder = new TextEncoder();

  // Build a flat strings buffer containing all keys and values.
  // Collect encoded segments first, then compute offsets.
  const segments: Uint8Array[] = [];
  const headerDescs: Array<{
    keyPtr: number;
    keyLen: number;
    valPtr: number;
    valLen: number;
  }> = [];

  let cursor = 0;
  for (const [key, val] of headers) {
    const keyBytes = encoder.encode(key);
    const valBytes = encoder.encode(val);
    headerDescs.push({
      keyPtr: cursor,
      keyLen: keyBytes.length,
      valPtr: cursor + keyBytes.length,
      valLen: valBytes.length,
    });
    cursor += keyBytes.length + valBytes.length;
    segments.push(keyBytes, valBytes);
  }

  // Assemble strings buffer
  const stringsTotal = cursor;
  const stringsBuf = new Uint8Array(stringsTotal);
  let off = 0;
  for (const seg of segments) {
    stringsBuf.set(seg, off);
    off += seg.length;
  }

  // Allocate WASM heap buffers — all inside a single try/finally so that
  // every non-zero pointer is freed even if a later allocation or the C call
  // throws.  (If copyToHeap for bodyPtr threw before this block, stringsBufPtr
  // would have been leaked; the try/finally below closes that window.)
  let stringsBufPtr = 0;
  let bodyPtr = 0;
  let structsPtr = 0;
  let outPtr = 0;
  let result: Uint8Array;

  try {
    stringsBufPtr = copyToHeap(mod, stringsBuf);
    bodyPtr = body.length > 0 ? copyToHeap(mod, body) : 0;

    structsPtr = mod._malloc(headers.length * HEADER_STRUCT_SIZE || 1);
    if (structsPtr === 0) {
      throw new Error("culpeostream-wasm: malloc failed for header structs");
    }

    // Calculate required output size (match C formula):
    // per header: key_len + 2 + val_len + 2; plus 2 (\r\n); plus body_len
    let outCap = 2 + body.length;
    for (const d of headerDescs) {
      outCap += d.keyLen + 2 + d.valLen + 2;
    }
    outPtr = mod._malloc(outCap || 2);
    if (outPtr === 0) {
      throw new Error("culpeostream-wasm: malloc failed for output buffer");
    }

    // Write header structs into WASM heap
    for (let i = 0; i < headerDescs.length; i++) {
      const d = headerDescs[i]!;
      const base = structsPtr + i * HEADER_STRUCT_SIZE;
      mod.setValue(base + 0, d.keyPtr, "i32");
      mod.setValue(base + 4, d.keyLen, "i32");
      mod.setValue(base + 8, d.valPtr, "i32");
      mod.setValue(base + 12, d.valLen, "i32");
    }

    const written = mod._culpeo_serialize_frame(
      structsPtr,
      headers.length,
      stringsBufPtr,
      stringsBuf.byteLength,
      bodyPtr,
      body.length,
      outPtr,
      outCap,
    );

    if (written < 0) {
      throw new Error("culpeostream-wasm: serialize_frame buffer too small");
    }

    result = mod.HEAPU8.slice(outPtr, outPtr + written);
  } finally {
    if (outPtr) mod._free(outPtr);
    if (structsPtr) mod._free(structsPtr);
    if (bodyPtr) mod._free(bodyPtr);
    if (stringsBufPtr) mod._free(stringsBufPtr);
  }

  return result;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * Build a `ParserBackend` that delegates to the currently loaded WASM module.
 *
 * Must only be called after a successful `loadWasmModule()`.
 */
export function createWasmParserBackend(): ParserBackend {
  return {
    parseHeaders: parseHeadersWasm,
    serializeFrame: serializeFrameWasm,
  };
}

/**
 * Load `dist/culpeo_parser.js` and initialise the Emscripten module.
 *
 * Returns `true` on success, `false` if the module is a stub (Emscripten not
 * compiled yet) or fails to load.  Never throws.
 *
 * @param wasmUrl  Optional URL/path to the `.wasm` file.  When omitted the
 *                 Emscripten glue file resolves it automatically.
 */
export async function loadWasmModule(wasmUrl?: string): Promise<boolean> {
  try {
    // Dynamic import so bundle tools can tree-shake the WASM path entirely
    // when this package is not used.
    const { createCulpeoParserModule } =
      await import("../dist/culpeo_parser.js");

    const mod: CulpeoParserModule = await createCulpeoParserModule(
      wasmUrl !== undefined ? { locateFile: () => wasmUrl } : undefined,
    );

    if (mod.__culpeoStub === true) {
      // Stub file committed for CI — WASM not compiled.
      return false;
    }

    wasmModule = mod;
    return true;
  } catch {
    return false;
  }
}

/**
 * Return `true` if a real (non-stub) WASM module is currently loaded.
 */
export function isWasmLoaded(): boolean {
  return wasmModule !== null;
}

/**
 * Unload the WASM module (for testing / cleanup).  Not part of the public
 * production API.
 *
 * @internal
 */
export function _unloadWasmModule(): void {
  wasmModule = null;
}
