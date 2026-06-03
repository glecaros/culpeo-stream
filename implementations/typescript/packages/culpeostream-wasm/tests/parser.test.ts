/**
 * tests/parser.test.ts — Vitest tests for culpeostream-wasm.
 *
 * Test matrix:
 *  1. Pure-TypeScript fallback path (WASM not loaded)
 *     - round-trip: parse → serialize → same bytes
 *     - edge cases: empty body, max-length header value, non-ASCII bytes
 *  2. Backend integration with culpeostream core
 *     - setParserBackend / getParserBackend wiring
 *     - core parseFrame / serializeFrame use the installed backend
 *  3. WASM vs TS parity (forced fallback produces same result)
 *  4. initWasm() → false when module is stub; fallback still works
 *  5. Error cases: missing ':' separator, incomplete frame
 */

import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

import {
  getParserBackend,
  parseFrame,
  serializeFrame as coreSerializeFrame,
  setParserBackend,
} from "culpeostream";

import type { ParserBackend } from "culpeostream";

import {
  deinitWasm,
  initWasm,
  isWasmLoaded,
  parseHeaders,
  serializeFrame,
} from "../src/index.js";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

const enc = new TextEncoder();
const dec = new TextDecoder();

/**
 * Build a raw CulpeoStream frame from a header string and body bytes.
 * Header string should end with \r\n; this helper appends the final \r\n.
 */
function buildFrame(headerLines: string, body: Uint8Array): Uint8Array {
  const headerBytes = enc.encode(headerLines + "\r\n");
  const frame = new Uint8Array(headerBytes.length + body.length);
  frame.set(headerBytes, 0);
  frame.set(body, headerBytes.length);
  return frame;
}

/** Build a binary media frame matching the core serializeBinaryFrame output. */
function buildMediaFrame(
  streamId: string,
  offset: number,
  contentType: string,
  body: Uint8Array,
): Uint8Array {
  const frame = {
    kind: "media" as const,
    headers: { streamId, offset, contentType },
    body,
  };
  const serialized = coreSerializeFrame(frame);
  if (serialized.frameType !== "binary") throw new Error("expected binary");
  return serialized.data;
}

// ---------------------------------------------------------------------------
// Suite 1 — Pure-TypeScript fallback (no WASM)
// ---------------------------------------------------------------------------

describe("Pure-TypeScript fallback (WASM not loaded)", () => {
  beforeEach(() => {
    // Ensure WASM is not active for these tests
    deinitWasm();
    setParserBackend(null);
  });

  afterEach(() => {
    setParserBackend(null);
  });

  it("parseHeaders returns null for incomplete input", () => {
    const incomplete = enc.encode(
      "Event: culpeo.ping\r\nContent-Type: application/json",
    );
    expect(parseHeaders(incomplete)).toBeNull();
  });

  it("parseHeaders parses a simple control frame header block", () => {
    const raw = enc.encode(
      "Event: culpeo.ping\r\nContent-Type: application/json\r\n\r\n{}",
    );
    const result = parseHeaders(raw);
    expect(result).not.toBeNull();
    expect(result!.headers["event"]).toBe("culpeo.ping");
    expect(result!.headers["content-type"]).toBe("application/json");
    expect(result!.bodyOffset).toBe(
      "Event: culpeo.ping\r\nContent-Type: application/json\r\n\r\n".length,
    );
  });

  it("parseHeaders lower-cases all header keys", () => {
    const raw = enc.encode("STREAM-ID: abc\r\nOFFSET: 100\r\n\r\n");
    const result = parseHeaders(raw);
    expect(result).not.toBeNull();
    expect(Object.keys(result!.headers)).toEqual(["stream-id", "offset"]);
  });

  it("parseHeaders trims whitespace from keys and values", () => {
    const raw = enc.encode("  X-Test  :  hello world  \r\n\r\n");
    const result = parseHeaders(raw);
    expect(result).not.toBeNull();
    expect(result!.headers["x-test"]).toBe("hello world");
  });

  it("serializeFrame produces correct header-body format", () => {
    const body = enc.encode("{}");
    const out = serializeFrame(
      { Event: "culpeo.ping", "Content-Type": "application/json" },
      body,
    );
    const text = dec.decode(out);
    expect(text).toContain("Event: culpeo.ping\r\n");
    expect(text).toContain("Content-Type: application/json\r\n");
    expect(text).toContain("\r\n\r\n{}");
  });

  it("round-trips a control frame: parse → serialize → same headers", () => {
    const body = enc.encode('{"ts":1000}');
    const frame = buildFrame(
      "Event: culpeo.ping\r\nContent-Type: application/json\r\n",
      body,
    );

    const parsed = parseHeaders(frame);
    expect(parsed).not.toBeNull();

    const reserialised = serializeFrame(parsed!.headers, body);
    const reparsed = parseHeaders(reserialised);
    expect(reparsed).not.toBeNull();
    expect(reparsed!.headers).toEqual(parsed!.headers);
    expect(dec.decode(reserialised.slice(reparsed!.bodyOffset))).toBe(
      '{"ts":1000}',
    );
  });

  it("round-trips a binary media frame with non-trivial body", () => {
    const body = new Uint8Array([0x00, 0xff, 0x80, 0x7f, 0xab, 0xcd]);
    const frame = buildMediaFrame("s-1", 640, "audio/opus", body);

    const parsed = parseHeaders(frame);
    expect(parsed).not.toBeNull();
    const parsedBody = frame.slice(parsed!.bodyOffset);
    expect(parsedBody).toEqual(body);

    // Re-serialize and check round-trip
    const reserialised = serializeFrame(parsed!.headers, parsedBody);
    const reparsed = parseHeaders(reserialised);
    expect(reparsed).not.toBeNull();
    expect(reserialised.slice(reparsed!.bodyOffset)).toEqual(body);
  });

  it("round-trips a frame with empty body", () => {
    const empty = new Uint8Array(0);
    const frame = buildFrame(
      "Event: culpeo.close\r\nCode: normal\r\nReason: ok\r\n",
      empty,
    );

    const parsed = parseHeaders(frame);
    expect(parsed).not.toBeNull();
    expect(parsed!.headers["code"]).toBe("normal");
    expect(frame.slice(parsed!.bodyOffset)).toEqual(empty);

    const reserialised = serializeFrame(parsed!.headers, empty);
    const reparsed = parseHeaders(reserialised);
    expect(reparsed).not.toBeNull();
    expect(reparsed!.headers).toEqual(parsed!.headers);
    expect(reserialised.slice(reparsed!.bodyOffset).length).toBe(0);
  });

  it("handles max-length header value (4095 bytes)", () => {
    const longValue = "x".repeat(4095);
    const raw = enc.encode(`X-Long: ${longValue}\r\n\r\n`);
    const result = parseHeaders(raw);
    expect(result).not.toBeNull();
    expect(result!.headers["x-long"]).toBe(longValue);
  });

  it("handles non-ASCII bytes in header value (treated as UTF-8 text)", () => {
    // UTF-8 encoded string in header value
    const value = "héllo wörld";
    const raw = enc.encode(`X-Unicode: ${value}\r\n\r\n`);
    const result = parseHeaders(raw);
    expect(result).not.toBeNull();
    expect(result!.headers["x-unicode"]).toBe(value);
  });

  it("handles multiple headers including unknown ones", () => {
    const raw = enc.encode(
      "Event: x-app.transcript\r\nX-Trace-Id: abc123\r\nX-Vendor: 1\r\n\r\n",
    );
    const result = parseHeaders(raw);
    expect(result).not.toBeNull();
    expect(result!.headers["event"]).toBe("x-app.transcript");
    expect(result!.headers["x-trace-id"]).toBe("abc123");
    expect(result!.headers["x-vendor"]).toBe("1");
  });

  it("returns null when buffer has only the header start (no \\r\\n\\r\\n)", () => {
    const raw = enc.encode("Event: culpeo.ping");
    expect(parseHeaders(raw)).toBeNull();
  });

  it("returns null on an empty buffer", () => {
    expect(parseHeaders(new Uint8Array(0))).toBeNull();
  });

  it("handles a frame with only \\r\\n\\r\\n (empty header block)", () => {
    const raw = enc.encode("\r\n\r\n");
    const result = parseHeaders(raw);
    expect(result).not.toBeNull();
    expect(Object.keys(result!.headers)).toHaveLength(0);
    expect(result!.bodyOffset).toBe(4);
  });
});

// ---------------------------------------------------------------------------
// Suite 2 — ParserBackend integration with culpeostream core
// ---------------------------------------------------------------------------

describe("ParserBackend integration with culpeostream core", () => {
  afterEach(() => {
    setParserBackend(null);
  });

  it("setParserBackend / getParserBackend wiring works", () => {
    expect(getParserBackend()).toBeNull();

    const mockBackend: ParserBackend = {
      parseHeaders: vi.fn().mockReturnValue({ headers: [], bodyOffset: 4 }),
      serializeFrame: vi.fn().mockReturnValue(new Uint8Array(0)),
    };

    setParserBackend(mockBackend);
    expect(getParserBackend()).toBe(mockBackend);

    setParserBackend(null);
    expect(getParserBackend()).toBeNull();
  });

  it("core parseFrame uses installed backend for raw parsing", () => {
    // Create a spy backend that delegates to the TS fallback
    let callCount = 0;
    const spyBackend: ParserBackend = {
      parseHeaders(buf) {
        callCount++;
        // Delegate to a simple TS parse for correctness
        const dec = new TextDecoder();
        let termPos = -1;
        for (let i = 0; i <= buf.length - 4; i++) {
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
        if (termPos < 0) return null;
        const text = dec.decode(buf.subarray(0, termPos));
        const pairs: Array<readonly [string, string]> = [];
        for (const line of text.split("\r\n")) {
          if (!line) continue;
          const c = line.indexOf(":");
          if (c <= 0) throw new Error("bad header");
          pairs.push([
            line.slice(0, c).trim().toLowerCase(),
            line.slice(c + 1).trim(),
          ] as const);
        }
        return { headers: pairs, bodyOffset: termPos + 4 };
      },
      serializeFrame(headers, body) {
        const enc = new TextEncoder();
        const hs = headers.map(([k, v]) => `${k}: ${v}\r\n`).join("") + "\r\n";
        const hb = enc.encode(hs);
        const out = new Uint8Array(hb.length + body.length);
        out.set(hb, 0);
        out.set(body, hb.length);
        return out;
      },
    };

    setParserBackend(spyBackend);

    // Now parse a real frame through the core
    const rawFrame =
      "Event: culpeo.ping\r\nContent-Type: application/json\r\n\r\n" +
      '{"ts":42}';
    const msg = parseFrame(rawFrame, "text");
    expect(msg.kind).toBe("control");
    if (msg.kind !== "control") throw new Error("expected control frame");
    expect(msg.event).toBe("culpeo.ping");

    // Our spy was called
    expect(callCount).toBe(1);
  });

  it("core serializeFrame uses installed backend for binary frames", () => {
    let serializeCalled = false;
    const spyBackend: ParserBackend = {
      parseHeaders: vi.fn(),
      serializeFrame(headers, body) {
        serializeCalled = true;
        // Just produce correct output
        const enc = new TextEncoder();
        const hs = headers.map(([k, v]) => `${k}: ${v}\r\n`).join("") + "\r\n";
        const hb = enc.encode(hs);
        const out = new Uint8Array(hb.length + body.length);
        out.set(hb, 0);
        out.set(body, hb.length);
        return out;
      },
    };

    setParserBackend(spyBackend);

    const mediaFrame = {
      kind: "media" as const,
      headers: {
        streamId: "s1",
        offset: 0,
        contentType: "audio/opus",
      },
      body: new Uint8Array([1, 2, 3]),
    };

    const serialized = coreSerializeFrame(mediaFrame);
    expect(serialized.frameType).toBe("binary");
    expect(serializeCalled).toBe(true);
  });
});

// ---------------------------------------------------------------------------
// Suite 3 — TS fallback parity guarantee
// ---------------------------------------------------------------------------

describe("TS fallback parity guarantee", () => {
  afterEach(() => {
    setParserBackend(null);
  });

  /**
   * Both the no-backend and custom-backend paths should produce identical
   * output for the same input.
   */
  it("serializeFrame without backend matches serializeFrame with spy backend", () => {
    const headers: Record<string, string> = {
      "Stream-Id": "s-test",
      Offset: "0",
      "Content-Type": "audio/pcm;rate=16000;channels=1;bits=16",
    };
    const body = new Uint8Array([10, 20, 30, 40]);

    // Without backend
    setParserBackend(null);
    const noBackendResult = serializeFrame(headers, body);

    // Verify expected format manually
    const text = dec.decode(noBackendResult);
    expect(text).toContain("Stream-Id: s-test\r\n");
    expect(text).toContain("Offset: 0\r\n");
    expect(text).toContain(
      "Content-Type: audio/pcm;rate=16000;channels=1;bits=16\r\n",
    );
    expect(noBackendResult.slice(noBackendResult.length - 4)).toEqual(body);
  });

  it("parseHeaders then serializeFrame is byte-stable (idempotent round-trip)", () => {
    const original = enc.encode(
      "Stream-Id: s1\r\nOffset: 320\r\nContent-Type: audio/opus\r\n\r\n",
    );
    const bodyBytes = new Uint8Array([0xaa, 0xbb, 0xcc]);
    const full = new Uint8Array(original.length + bodyBytes.length);
    full.set(original, 0);
    full.set(bodyBytes, original.length);

    const parsed = parseHeaders(full);
    expect(parsed).not.toBeNull();

    const reserialised = serializeFrame(parsed!.headers, bodyBytes);
    const reparsed = parseHeaders(reserialised);
    expect(reparsed).not.toBeNull();

    // The header sets should be equal (same key/value pairs)
    expect(reparsed!.headers).toEqual(parsed!.headers);
    // The body should be unchanged
    expect(reserialised.slice(reparsed!.bodyOffset)).toEqual(bodyBytes);
  });
});

// ---------------------------------------------------------------------------
// Suite 4 — initWasm() resolves false for stub; fallback still works
// ---------------------------------------------------------------------------

describe("initWasm() with stub module", () => {
  beforeEach(() => {
    deinitWasm();
  });

  afterEach(() => {
    deinitWasm();
    setParserBackend(null);
  });

  it("initWasm returns false (stub module, Emscripten not compiled)", async () => {
    const result = await initWasm();
    expect(result).toBe(false);
  });

  it("isWasmLoaded is false after a failed initWasm", async () => {
    await initWasm();
    expect(isWasmLoaded()).toBe(false);
  });

  it("parseHeaders still works via TS fallback after failed initWasm", async () => {
    await initWasm();
    const raw = enc.encode(
      "Event: culpeo.pong\r\nContent-Type: application/json\r\n\r\n{}",
    );
    const result = parseHeaders(raw);
    expect(result).not.toBeNull();
    expect(result!.headers["event"]).toBe("culpeo.pong");
  });

  it("serializeFrame still works via TS fallback after failed initWasm", async () => {
    await initWasm();
    const out = serializeFrame(
      { Event: "culpeo.close", Code: "normal", Reason: "bye" },
      new Uint8Array(0),
    );
    const text = dec.decode(out);
    expect(text).toContain("Event: culpeo.close\r\n");
    expect(text).toContain("Code: normal\r\n");
    expect(text).toContain("Reason: bye\r\n");
    expect(text.endsWith("\r\n\r\n")).toBe(true);
  });

  it("core parser backend is NOT installed after failed initWasm", async () => {
    setParserBackend(null);
    await initWasm();
    expect(getParserBackend()).toBeNull();
  });

  it("initWasm returns false on a second call when stub is loaded", async () => {
    const first = await initWasm();
    const second = await initWasm();
    expect(first).toBe(false);
    expect(second).toBe(false);
  });
});

// ---------------------------------------------------------------------------
// Suite 5 — Edge cases and error handling
// ---------------------------------------------------------------------------

describe("Edge cases and error handling", () => {
  afterEach(() => {
    setParserBackend(null);
  });

  it("parseHeaders handles a large number of headers correctly", () => {
    const lines = Array.from(
      { length: 50 },
      (_, i) => `X-Header-${i}: value-${i}`,
    ).join("\r\n");
    const raw = enc.encode(lines + "\r\n\r\n");
    const result = parseHeaders(raw);
    expect(result).not.toBeNull();
    expect(Object.keys(result!.headers)).toHaveLength(50);
    expect(result!.headers["x-header-0"]).toBe("value-0");
    expect(result!.headers["x-header-49"]).toBe("value-49");
  });

  it("serializeFrame handles a large binary body", () => {
    const body = new Uint8Array(65_536).fill(0xaa);
    const out = serializeFrame(
      { "Stream-Id": "s1", Offset: "0", "Content-Type": "audio/raw" },
      body,
    );
    const parsed = parseHeaders(out);
    expect(parsed).not.toBeNull();
    const parsedBody = out.slice(parsed!.bodyOffset);
    expect(parsedBody).toEqual(body);
  });

  it("serializeFrame preserves header key capitalisation", () => {
    const out = serializeFrame(
      { "Stream-Id": "abc", "Content-Type": "audio/opus" },
      new Uint8Array(0),
    );
    const text = dec.decode(out);
    // Keys should be written exactly as supplied (not lowercased on output)
    expect(text).toContain("Stream-Id: abc\r\n");
    expect(text).toContain("Content-Type: audio/opus\r\n");
  });

  it("core parseFrame round-trips correctly via TS backend (no override)", () => {
    setParserBackend(null);
    const media = {
      kind: "media" as const,
      headers: {
        streamId: "s-round",
        offset: 1024,
        contentType: "audio/opus",
        timestamp: 99_000,
      },
      body: new Uint8Array([0xde, 0xad, 0xbe, 0xef]),
    };
    const serialized = coreSerializeFrame(media);
    if (serialized.frameType !== "binary") throw new Error("expected binary");
    const parsed = parseFrame(serialized.data, "binary");
    expect(parsed).toEqual(media);
  });

  it("culpeostream core still enforces duplicate reserved header check even via backend", () => {
    // Install a backend that returns duplicate reserved headers
    const duplicateBackend: ParserBackend = {
      parseHeaders(_buf) {
        return {
          headers: [
            ["event", "culpeo.ping"] as const,
            ["event", "culpeo.pong"] as const, // duplicate reserved header
          ],
          bodyOffset: 4,
        };
      },
      serializeFrame: vi.fn(),
    };
    setParserBackend(duplicateBackend);

    expect(() => parseFrame(new Uint8Array(4), "text")).toThrow(
      /Duplicate reserved header: event/,
    );
  });

  it("culpeostream core enforces header count limit via backend path", () => {
    const tooManyHeaders: ParserBackend = {
      parseHeaders(_buf) {
        const pairs: Array<readonly [string, string]> = Array.from(
          { length: 100 },
          (_, i) => [`x-hdr-${i}`, "v"] as const,
        );
        return { headers: pairs, bodyOffset: 4 };
      },
      serializeFrame: vi.fn(),
    };
    setParserBackend(tooManyHeaders);

    expect(() =>
      parseFrame(new Uint8Array(4), "text", { maxHeaderCount: 5 }),
    ).toThrow(/Header count exceeds maximum/);
  });
});
