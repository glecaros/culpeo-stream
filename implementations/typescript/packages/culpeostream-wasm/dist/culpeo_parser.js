/**
 * culpeo_parser.js — STUB (not compiled from C source)
 *
 * This file is committed to enable CI runs without Emscripten.  It presents
 * the same module shape as the real Emscripten-compiled output but every
 * exported function signals "not compiled" by returning a sentinel object
 * recognised by wasm-loader.ts.
 *
 * To produce the real WASM module see WASM_BUILD.md.
 */

/** @returns {Promise<{__culpeoStub: true}>} */
export async function createCulpeoParserModule() {
  return {
    /** Sentinel recognised by wasm-loader.ts to mean "stub, use TS fallback". */
    __culpeoStub: true,

    /** Stub _malloc — always returns 0 (null pointer). */
    _malloc(_size) {
      return 0;
    },

    /** Stub _free — no-op. */
    _free(_ptr) {},

    /** Stub _culpeo_parse_headers — signals "not compiled". */
    _culpeo_parse_headers(_buf, _len, _headersOut, _maxHeaders, _bodyOffsetOut) {
      return -1;
    },

    /** Stub _culpeo_serialize_frame — signals "not compiled". */
    _culpeo_serialize_frame(
      _headers,
      _headerCount,
      _stringsBuf,
      _body,
      _bodyLen,
      _outBuf,
      _outCap,
    ) {
      return -1;
    },

    /** Stub HEAPU8 view — zero-length (safe to read from, never written). */
    HEAPU8: new Uint8Array(0),

    /** Stub getValue — always returns 0. */
    getValue(_ptr, _type) {
      return 0;
    },

    /** Stub setValue — no-op. */
    setValue(_ptr, _value, _type) {},
  };
}

export default createCulpeoParserModule;
