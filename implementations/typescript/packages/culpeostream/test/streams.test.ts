import { describe, expect, it } from "vitest";

import { StreamRegistry, validateStreamDeclarations } from "../src/streams.js";

describe("stream registry", () => {
  it("rejects duplicate purposes within the same direction", () => {
    expect(() =>
      validateStreamDeclarations([
        {
          content_type: "audio/pcm;rate=16000;channels=1;bits=16",
          type: "input",
          purpose: "voice",
        },
        { content_type: "audio/opus", type: "input", purpose: "voice" },
      ]),
    ).toThrow(/Duplicate purpose/);
  });

  it("enforces directionality on send and receive", () => {
    const registry = new StreamRegistry("client", [
      {
        content_type: "audio/pcm;rate=16000;channels=1;bits=16",
        type: "input",
      },
      { content_type: "audio/opus", type: "output" },
    ]);
    registry.confirmFromAck([
      {
        id: "s-in",
        content_type: "audio/pcm;rate=16000;channels=1;bits=16",
        type: "input",
      },
      { id: "s-out", content_type: "audio/opus", type: "output" },
    ]);

    expect(registry.trackSend("s-in", 640).offset).toBe(0);
    expect(() => registry.trackSend("s-out", 100)).toThrow(/direction/);
    expect(() =>
      registry.trackReceive(
        "s-in",
        0,
        "audio/pcm;rate=16000;channels=1;bits=16",
        640,
      ),
    ).toThrow(/direction/);
    expect(() =>
      registry.trackReceive("s-out", 0, "audio/opus", 100),
    ).not.toThrow();
  });

  it("builds resume declarations from a previous snapshot", () => {
    const registry = new StreamRegistry("client", [
      { content_type: "audio/opus", type: "output", purpose: "assistant" },
    ]);
    const streams = registry.buildInitStreams({
      sessionId: "session-1",
      version: "0.3",
      bufferWindowMs: 5_000,
      streams: [
        {
          id: "stream-1",
          content_type: "audio/opus",
          type: "output",
          purpose: "assistant",
          resume_offset: 42,
        },
      ],
    });

    expect(streams).toEqual([
      {
        id: "stream-1",
        content_type: "audio/opus",
        type: "output",
        purpose: "assistant",
        resume_offset: 42,
      },
    ]);
  });

  it("rejects stream declarations beyond the maximum count", () => {
    expect(() =>
      validateStreamDeclarations(
        Array.from({ length: 17 }, (_, index) => ({
          content_type: `audio/opus;variant=${index}`,
          type: "input" as const,
          purpose: `stream-${index}`,
        })),
      ),
    ).toThrow(/Maximum of 16 streams per session/);
  });
});
