import { describe, it, expect } from "vitest";
import {
  computeBackoffDelayMs,
  defaultReconnectOptions,
  shouldRetry,
} from "../src/reconnect.js";
import type { ReconnectOptions } from "../src/reconnect.js";

// A deterministic random source that always returns 0.5
const half = () => 0.5;
// A deterministic random source that always returns 0
const zero = () => 0;
// A deterministic random source that always returns the maximum (just under 1)
const max = () => 0.9999999;

const opts: ReconnectOptions = {
  maxAttempts: 5,
  baseDelayMs: 1000,
  maxDelayMs: 30_000,
};

describe("shouldRetry", () => {
  it("allows retries below maxAttempts", () => {
    expect(shouldRetry(0, opts)).toBe(true);
    expect(shouldRetry(4, opts)).toBe(true);
  });

  it("disallows retries at or above maxAttempts", () => {
    expect(shouldRetry(5, opts)).toBe(false);
    expect(shouldRetry(10, opts)).toBe(false);
  });

  it("always retries when maxAttempts is Infinity", () => {
    expect(shouldRetry(1_000_000, defaultReconnectOptions)).toBe(true);
  });

  it("never retries when maxAttempts is 0", () => {
    expect(shouldRetry(0, { ...opts, maxAttempts: 0 })).toBe(false);
  });
});

describe("computeBackoffDelayMs", () => {
  it("returns 0 when the random source returns 0", () => {
    expect(computeBackoffDelayMs(0, opts, zero)).toBe(0);
    expect(computeBackoffDelayMs(3, opts, zero)).toBe(0);
  });

  it("caps at maxDelayMs regardless of attempt number", () => {
    // attempt 100: base * 2^100 >> maxDelayMs
    const delay = computeBackoffDelayMs(100, opts, max);
    expect(delay).toBeLessThanOrEqual(opts.maxDelayMs);
  });

  it("grows exponentially up to the cap with half-random source", () => {
    const d0 = computeBackoffDelayMs(0, opts, half); // ~500
    const d1 = computeBackoffDelayMs(1, opts, half); // ~1000
    const d2 = computeBackoffDelayMs(2, opts, half); // ~2000
    const d3 = computeBackoffDelayMs(3, opts, half); // ~4000

    expect(d1).toBeGreaterThan(d0);
    expect(d2).toBeGreaterThan(d1);
    expect(d3).toBeGreaterThan(d2);
  });

  it("at attempt 0, cap is baseDelayMs", () => {
    // cap = min(30000, 1000 * 2^0) = 1000; with half random: floor(0.5 * 1000) = 500
    expect(computeBackoffDelayMs(0, opts, half)).toBe(500);
  });

  it("at attempt 1, cap is 2 * baseDelayMs", () => {
    // cap = min(30000, 1000 * 2^1) = 2000; with half: 1000
    expect(computeBackoffDelayMs(1, opts, half)).toBe(1000);
  });

  it("at attempt 5, cap is clamped to maxDelayMs", () => {
    // 1000 * 2^5 = 32000 > 30000; cap = 30000; with half: 15000
    expect(computeBackoffDelayMs(5, opts, half)).toBe(15_000);
  });

  it("returns an integer (floor applied)", () => {
    // With a non-trivial random source
    const result = computeBackoffDelayMs(2, opts, () => 0.3);
    expect(Number.isInteger(result)).toBe(true);
  });

  it("respects a custom baseDelayMs", () => {
    const custom: ReconnectOptions = { maxAttempts: 10, baseDelayMs: 500, maxDelayMs: 10_000 };
    // attempt 0: cap = 500; with half: 250
    expect(computeBackoffDelayMs(0, custom, half)).toBe(250);
  });
});
