/**
 * culpeostream-wasm — WebAssembly-accelerated CulpeoStream header parser
 * and serializer with transparent TypeScript fallback.
 *
 * ## Quick start
 *
 * ```typescript
 * import { initWasm, parseHeaders, serializeFrame } from "culpeostream-wasm";
 *
 * // Call once at startup.  Returns true when WASM loaded, false → TS fallback.
 * const usingWasm = await initWasm();
 *
 * // Use the parse / serialize helpers as a direct (non-session) utility:
 * const result = parseHeaders(someBytes);
 * const frame  = serializeFrame({ "Event": "culpeo.ping" }, new Uint8Array(0));
 * ```
 *
 * ## Integration with the culpeostream core
 *
 * After `initWasm()` returns `true` the package automatically installs itself
 * as the active `ParserBackend` in the culpeostream core via
 * `setParserBackend()`.  From that point all `parseFrame` / `serializeFrame`
 * calls made by the core will go through WASM.
 *
 * If WASM fails to load (or is unavailable at runtime) the core continues
 * using its built-in pure-TypeScript parser with no change to behaviour.
 */

import { setParserBackend } from "culpeostream";
import {
  _unloadWasmModule,
  createWasmParserBackend,
  isWasmLoaded,
  loadWasmModule,
} from "./wasm-loader.js";

export type { ParserBackend, ParseLimits } from "./types.js";
export { isWasmLoaded } from "./wasm-loader.js";

// ---------------------------------------------------------------------------
// Module-level fallback (pure-TypeScript)
// ---------------------------------------------------------------------------

const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder();

/**
 * Pure-TypeScript implementation of header parsing.
 * Used when WASM is unavailable.
 */
function parseHeadersTs(
  buf: Uint8Array,
): {
  headers: ReadonlyArray<readonly [string, string]>;
  bodyOffset: number;
} | null {
  // Locate \r\n\r\n
  let termPos = -1;
  const searchLen = Math.min(buf.length, buf.length);
  for (let i = 0; i <= searchLen - 4; i++) {
    if (
      buf[i] === 13 &&
      buf[i + 1] === 10 &&
      buf[i + 2] === 13 &&
      buf[i + 3] === 10
    ) {
      termPos = i;
      break;
    }
  }
  if (termPos < 0) {
    return null;
  }

  const headerText = textDecoder.decode(buf.subarray(0, termPos));
  const lines = headerText.length === 0 ? [] : headerText.split("\r\n");
  const pairs: Array<readonly [string, string]> = [];

  for (const line of lines) {
    if (line.length === 0) continue;
    const colon = line.indexOf(":");
    if (colon <= 0) {
      throw new Error(
        `culpeostream-wasm: malformed header line (missing ':'): ${line}`,
      );
    }
    const key = line.slice(0, colon).trim().toLowerCase();
    const val = line.slice(colon + 1).trim();
    pairs.push([key, val] as const);
  }

  return { headers: pairs, bodyOffset: termPos + 4 };
}

/**
 * Pure-TypeScript implementation of frame serialization.
 * Used when WASM is unavailable.
 */
function serializeFrameTs(
  headers: ReadonlyArray<readonly [string, string]>,
  body: Uint8Array,
): Uint8Array {
  const headerStr = headers.map(([k, v]) => `${k}: ${v}\r\n`).join("") + "\r\n";
  const headerBytes = textEncoder.encode(headerStr);
  const result = new Uint8Array(headerBytes.length + body.length);
  result.set(headerBytes, 0);
  result.set(body, headerBytes.length);
  return result;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * Initialise the WASM module.
 *
 * - On success (`true`): the WASM backend is installed in the culpeostream core
 *   via `setParserBackend()`.  All subsequent `parseFrame` / `serializeFrame`
 *   calls in the core will use WASM.
 * - On failure (`false`): the core's built-in pure-TypeScript parser remains
 *   active.  `parseHeaders()` / `serializeFrame()` exported by this module
 *   fall back to their internal TS implementations.
 *
 * Safe to call multiple times; subsequent calls are no-ops if already loaded.
 *
 * @param wasmUrl  Optional explicit URL/path for the `.wasm` binary.  Useful
 *                 when serving from a CDN or non-default path.
 * @returns        `true` if WASM is now active, `false` if falling back to TS.
 */
export async function initWasm(wasmUrl?: string): Promise<boolean> {
  if (isWasmLoaded()) {
    return true;
  }
  const ok = await loadWasmModule(wasmUrl);
  if (ok) {
    setParserBackend(createWasmParserBackend());
  }
  return ok;
}

/**
 * Parse a CulpeoStream header block from raw bytes.
 *
 * Uses the WASM implementation when `initWasm()` has previously returned
 * `true`, otherwise falls back to the built-in TypeScript parser.  Both paths
 * produce identical output.
 *
 * The returned `headers` object uses lower-cased, trimmed keys.
 *
 * @returns `null` when the `\r\n\r\n` terminator is absent (incomplete data).
 */
export function parseHeaders(
  buf: Uint8Array,
): { headers: Record<string, string>; bodyOffset: number } | null {
  const impl = isWasmLoaded() ? createWasmParserBackend() : null;

  const result = impl !== null ? impl.parseHeaders(buf) : parseHeadersTs(buf);

  if (result === null) {
    return null;
  }

  const record: Record<string, string> = {};
  for (const [k, v] of result.headers) {
    record[k] = v;
  }
  return { headers: record, bodyOffset: result.bodyOffset };
}

/**
 * Serialize header key/value pairs and a body into a complete CulpeoStream
 * frame byte sequence.
 *
 * Format: `Key: Value\r\n` × N → `\r\n` → body
 *
 * Uses the WASM implementation when available, otherwise falls back to the
 * built-in TypeScript serializer.  Both paths produce byte-for-byte identical
 * output.
 */
export function serializeFrame(
  headers: Record<string, string>,
  body: Uint8Array,
): Uint8Array {
  const pairs = Object.entries(headers) as Array<readonly [string, string]>;
  const impl = isWasmLoaded() ? createWasmParserBackend() : null;

  return impl !== null
    ? impl.serializeFrame(pairs, body)
    : serializeFrameTs(pairs, body);
}

/**
 * Create a `ParserBackend` that uses the WASM implementation.
 *
 * Returns `null` when WASM has not been loaded (call `initWasm()` first).
 * Intended for advanced users who want to manage the backend lifecycle
 * directly rather than using the auto-install behaviour of `initWasm()`.
 */
export function createWasmBackend(): ReturnType<
  typeof createWasmParserBackend
> | null {
  if (!isWasmLoaded()) {
    return null;
  }
  return createWasmParserBackend();
}

/**
 * Uninstall the WASM backend from the culpeostream core and unload the module.
 *
 * After calling this function the core reverts to its built-in pure-TypeScript
 * parser.  Primarily useful in tests.
 */
export function deinitWasm(): void {
  setParserBackend(null);
  _unloadWasmModule();
}
