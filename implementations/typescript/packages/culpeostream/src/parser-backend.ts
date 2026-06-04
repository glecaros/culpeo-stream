/**
 * ParserBackend — pluggable low-level header parser / serializer.
 *
 * The default backend is the pure-TypeScript implementation that is always
 * available.  `culpeostream-wasm` replaces it after a successful
 * `initWasm()` call via `setParserBackend()`.
 *
 * The interface operates on raw bytes so that a WASM or native backend can
 * own the hot path while all semantic validation remains in TypeScript.
 */
export interface ParserBackend {
  /**
   * Scan `buf` for the `\r\n\r\n` header terminator, split each line on the
   * first `:`, lowercase the key, and trim both sides.
   *
   * Returns an **ordered** list of [key, value] pairs (preserving duplicates
   * for duplicate-reserved-header detection by the caller) together with the
   * byte offset at which the body begins.
   *
   * Returns `null` when the header terminator has not yet been seen
   * (incomplete frame).  Throws a `CulpeoError("protocol-error", …)` for
   * structurally malformed input (e.g. a line with no `:` separator).
   */
  parseHeaders(
    buf: Uint8Array,
  ): {
    headers: ReadonlyArray<readonly [string, string]>;
    bodyOffset: number;
  } | null;

  /**
   * Produce a complete frame byte sequence:
   *   `Key: Value\r\n` × N  →  `\r\n`  →  body bytes
   *
   * Header keys are written exactly as supplied (including capitalisation).
   */
  serializeFrame(
    headers: ReadonlyArray<readonly [string, string]>,
    body: Uint8Array,
  ): Uint8Array;
}

// ---------------------------------------------------------------------------
// Module-level active backend
// ---------------------------------------------------------------------------

let activeBackend: ParserBackend | null = null;

/**
 * Replace the active parser backend.
 *
 * Called by `culpeostream-wasm` after a successful `initWasm()`.  Passing
 * `null` reverts to the built-in pure-TypeScript implementation.
 */
export function setParserBackend(backend: ParserBackend | null): void {
  activeBackend = backend;
}

/**
 * Return the currently active backend, or `null` when no override has been
 * registered (pure-TS fallback will be used).
 */
export function getParserBackend(): ParserBackend | null {
  return activeBackend;
}
