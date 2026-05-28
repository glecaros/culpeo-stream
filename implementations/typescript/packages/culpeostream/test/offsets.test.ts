import { describe, expect, it } from "vitest";

import { computeOffsetIncrement, OffsetTracker } from "../src/offsets.js";

describe("offset tracking", () => {
  it("increments PCM streams by sample count", () => {
    expect(
      computeOffsetIncrement("audio/pcm;rate=16000;channels=1;bits=16", 640),
    ).toBe(320);
  });

  it("requires rate, channels, and bits for PCM streams", () => {
    expect(() =>
      computeOffsetIncrement("audio/pcm;channels=1;bits=16", 640),
    ).toThrow(/rate, channels, and bits/);
  });

  it("increments encoded streams by frame count", () => {
    expect(computeOffsetIncrement("audio/opus", 256)).toBe(1);
    expect(computeOffsetIncrement("audio/aac", 256)).toBe(1);
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
