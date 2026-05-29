import { describe, expect, it } from "vitest";

import { StreamRegistry, validateStreamDeclarations } from "../src/streams.js";

describe("stream registry", () => {
  it("rejects duplicate purposes within the same direction", () => {
    expect(() =>
      validateStreamDeclarations([
        {
          content_type: "audio/pcm;rate=16000;channels=1;bits=16",
          type: "input",
          offset_type: "time",
          purpose: "voice",
        },
        { content_type: "audio/opus", type: "input", offset_type: "message", purpose: "voice" },
      ]),
    ).toThrow(/Duplicate purpose/);
  });

  it("rejects stream declarations with missing offset_type", () => {
    expect(() =>
      validateStreamDeclarations([
        // Cast to bypass TS — simulates a runtime JSON parse without offset_type
        { content_type: "audio/opus", type: "input" } as never,
      ]),
    ).toThrow(/offset_type/);
  });

  it("rejects stream declarations with an unrecognised offset_type", () => {
    expect(() =>
      validateStreamDeclarations([
        {
          content_type: "audio/opus",
          type: "input",
          offset_type: "frames" as never,
        },
      ]),
    ).toThrow(/offset_type/);
  });

  it("enforces directionality on send and receive", () => {
    const registry = new StreamRegistry("client", [
      {
        content_type: "audio/pcm;rate=16000;channels=1;bits=16",
        type: "input",
        offset_type: "time",
      },
      { content_type: "audio/opus", type: "output", offset_type: "message" },
    ]);
    registry.confirmFromAck([
      {
        id: "s-in",
        content_type: "audio/pcm;rate=16000;channels=1;bits=16",
        type: "input",
        offset_type: "time",
      },
      { id: "s-out", content_type: "audio/opus", type: "output", offset_type: "message" },
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
      { content_type: "audio/opus", type: "output", offset_type: "message", purpose: "assistant" },
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
          offset_type: "message",
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
        offset_type: "message",
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
          offset_type: "message" as const,
          purpose: `stream-${index}`,
        })),
      ),
    ).toThrow(/Maximum of 16 streams per session/);
  });

  it("tracks byte-type stream offsets by payload byte length", () => {
    const registry = new StreamRegistry("client", [
      { content_type: "application/octet-stream", type: "input", offset_type: "byte" },
    ]);
    registry.confirmFromAck([
      { id: "s-byte", content_type: "application/octet-stream", type: "input", offset_type: "byte" },
    ]);

    const first = registry.trackSend("s-byte", 256);
    expect(first.offset).toBe(0);

    const second = registry.trackSend("s-byte", 512);
    expect(second.offset).toBe(256);

    const third = registry.trackSend("s-byte", 100);
    expect(third.offset).toBe(768);
  });
});
