import { describe, expect, it } from "vitest";

import { computeOffsetIncrement, OffsetTracker } from "../src/offsets.js";

describe("offset tracking", () => {
  it("increments PCM streams by sample count (offset_type: time)", () => {
    expect(
      computeOffsetIncrement("time", 640, "audio/pcm;rate=16000;channels=1;bits=16"),
    ).toBe(320);
  });

  it("requires rate, channels, and bits for time offset_type on PCM streams", () => {
    expect(() =>
      computeOffsetIncrement("time", 640, "audio/pcm;channels=1;bits=16"),
    ).toThrow(/rate, channels, and bits/);
  });

  it("requires a PCM content type when offset_type is time", () => {
    expect(() =>
      computeOffsetIncrement("time", 256, "audio/opus"),
    ).toThrow(/PCM content type/);
  });

  it("increments encoded streams by frame count (offset_type: message)", () => {
    expect(computeOffsetIncrement("message", 256)).toBe(1);
    expect(computeOffsetIncrement("message", 1024)).toBe(1);
  });

  it("increments byte streams by payload byte length (offset_type: byte)", () => {
    expect(computeOffsetIncrement("byte", 256)).toBe(256);
    expect(computeOffsetIncrement("byte", 1024)).toBe(1024);
    expect(computeOffsetIncrement("byte", 0)).toBe(0);
  });

  it("enforces strictly contiguous received offsets", () => {
    const tracker = new OffsetTracker();
    tracker.register("stream");
    tracker.recordReceived("stream", 0, 1);
    expect(() => tracker.recordReceived("stream", 2, 1)).toThrow(
      /strictly contiguous/,
    );
  });
});
