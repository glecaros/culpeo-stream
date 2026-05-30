/**
 * Reconnection backoff logic.
 *
 * Uses the "full jitter" strategy:
 *   delay = random(0, min(maxDelayMs, baseDelayMs × 2^attempt))
 *
 * This avoids thundering-herd while still converging quickly after short
 * outages. `Math.random()` is deliberately avoided — we use
 * `crypto.getRandomValues()` as required by the security spec.
 */

/** Reconnection configuration. All fields are required; use spread with `defaultReconnectOptions` for overrides. */
export interface ReconnectOptions {
  /**
   * Maximum number of reconnect attempts before giving up.
   * Set to `Infinity` (the default) to reconnect indefinitely.
   */
  maxAttempts: number;
  /** Base delay in milliseconds for the exponential envelope. Default: 1000. */
  baseDelayMs: number;
  /** Maximum delay cap in milliseconds. Default: 30 000. */
  maxDelayMs: number;
}

export const defaultReconnectOptions: Readonly<ReconnectOptions> = {
  maxAttempts: Infinity,
  baseDelayMs: 1000,
  maxDelayMs: 30_000,
};

/**
 * Computes the next backoff delay using full jitter over the exponential
 * envelope.
 *
 * @param attempt - Zero-based attempt number (0 = first reconnect).
 * @param options - Reconnect configuration.
 * @param randomFloat - Optional override for the [0,1) random source; defaults
 *   to a `crypto.getRandomValues()`-backed implementation.
 */
export function computeBackoffDelayMs(
  attempt: number,
  options: ReconnectOptions,
  randomFloat: () => number = secureRandom,
): number {
  // Clamp the exponential envelope to the configured maximum.
  const cap = Math.min(
    options.maxDelayMs,
    options.baseDelayMs * Math.pow(2, attempt),
  );
  return Math.floor(randomFloat() * cap);
}

/** Returns true when more reconnect attempts are permitted. */
export function shouldRetry(
  attempt: number,
  options: ReconnectOptions,
): boolean {
  return attempt < options.maxAttempts;
}

/**
 * Cryptographically random float in [0, 1).
 *
 * Uses `crypto.getRandomValues()` — never `Math.random()` — as required by
 * the security specification.
 */
function secureRandom(): number {
  const bytes = new Uint8Array(4);
  globalThis.crypto.getRandomValues(bytes);
  // Interpret as a big-endian uint32, then normalise to [0, 1).
  const value =
    (((bytes[0]! << 24) | (bytes[1]! << 16) | (bytes[2]! << 8) | bytes[3]!) >>>
      0) /
    0x1_0000_0000;
  return value;
}
