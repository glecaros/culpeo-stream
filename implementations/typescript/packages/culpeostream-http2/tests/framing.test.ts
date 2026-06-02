/**
 * framing.test.ts — Unit tests for HTTP/2 frame encoding/decoding.
 *
 * Covers:
 *  1. encodeFrame + decodeFrame round-trips a control frame
 *  2. encodeFrame + decodeFrame round-trips a media frame
 *  3. decodeFrame returns null when buffer is incomplete
 *  4. decodeFrame handles zero-length payload
 *  5. decodeFrame handles multi-frame buffer (two frames concatenated)
 */

import { describe, expect, it } from "vitest";
import {
  CONTROL_FRAME,
  MEDIA_FRAME,
  decodeFrame,
  encodeFrame,
} from "../src/framing.js";

describe("HTTP/2 framing", () => {
  it("encodeFrame + decodeFrame round-trips a control frame", () => {
    const payload = Buffer.from(
      "Event: culpeo.ping\r\nContent-Type: application/json\r\n\r\n{}",
    );
    const encoded = encodeFrame(CONTROL_FRAME, payload);

    // Should be 5 (header) + payload.length bytes
    expect(encoded.length).toBe(5 + payload.length);
    // First byte is type octet
    expect(encoded.readUInt8(0)).toBe(CONTROL_FRAME);
    // Next 4 bytes are big-endian length
    expect(encoded.readUInt32BE(1)).toBe(payload.length);

    const decoded = decodeFrame(encoded);
    expect(decoded).not.toBeNull();
    expect(decoded!.typeOctet).toBe(CONTROL_FRAME);
    expect(decoded!.bytesConsumed).toBe(encoded.length);
    expect(Buffer.compare(decoded!.payload, payload)).toBe(0);
  });

  it("encodeFrame + decodeFrame round-trips a media frame", () => {
    const payload = Buffer.alloc(256, 0xab);
    const encoded = encodeFrame(MEDIA_FRAME, payload);

    expect(encoded.readUInt8(0)).toBe(MEDIA_FRAME);
    expect(encoded.readUInt32BE(1)).toBe(256);

    const decoded = decodeFrame(encoded);
    expect(decoded).not.toBeNull();
    expect(decoded!.typeOctet).toBe(MEDIA_FRAME);
    expect(decoded!.bytesConsumed).toBe(5 + 256);
    expect(Buffer.compare(decoded!.payload, payload)).toBe(0);
  });

  it("decodeFrame returns null when buffer is incomplete", () => {
    const payload = Buffer.from("hello world");
    const encoded = encodeFrame(CONTROL_FRAME, payload);

    // Only give part of the buffer
    expect(decodeFrame(encoded.subarray(0, 3))).toBeNull(); // not even header
    expect(decodeFrame(encoded.subarray(0, 5))).toBeNull(); // header only, no payload
    expect(decodeFrame(encoded.subarray(0, encoded.length - 1))).toBeNull(); // one byte short
  });

  it("decodeFrame handles zero-length payload", () => {
    const payload = Buffer.alloc(0);
    const encoded = encodeFrame(CONTROL_FRAME, payload);

    expect(encoded.length).toBe(5);
    expect(encoded.readUInt32BE(1)).toBe(0);

    const decoded = decodeFrame(encoded);
    expect(decoded).not.toBeNull();
    expect(decoded!.typeOctet).toBe(CONTROL_FRAME);
    expect(decoded!.payload.length).toBe(0);
    expect(decoded!.bytesConsumed).toBe(5);
  });

  it("decodeFrame handles multi-frame buffer (two frames concatenated)", () => {
    const payload1 = Buffer.from("frame-one");
    const payload2 = Buffer.from("frame-two-longer-payload");
    const encoded1 = encodeFrame(CONTROL_FRAME, payload1);
    const encoded2 = encodeFrame(MEDIA_FRAME, payload2);

    const combined = Buffer.concat([encoded1, encoded2]);

    // Decode first frame
    const first = decodeFrame(combined);
    expect(first).not.toBeNull();
    expect(first!.typeOctet).toBe(CONTROL_FRAME);
    expect(Buffer.compare(first!.payload, payload1)).toBe(0);

    // Decode second frame from remaining bytes
    const remaining = combined.subarray(first!.bytesConsumed);
    const second = decodeFrame(remaining);
    expect(second).not.toBeNull();
    expect(second!.typeOctet).toBe(MEDIA_FRAME);
    expect(Buffer.compare(second!.payload, payload2)).toBe(0);
    expect(second!.bytesConsumed).toBe(5 + payload2.length);
  });

  it("decodeFrame throws RangeError when payload exceeds maxPayloadBytes", () => {
    const payload = Buffer.alloc(100);
    const encoded = encodeFrame(CONTROL_FRAME, payload);
    // Allow only 50 bytes max
    expect(() => decodeFrame(encoded, 50)).toThrow(RangeError);
  });
});
