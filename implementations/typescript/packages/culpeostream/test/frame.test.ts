import { describe, expect, it } from "vitest";

import { parseFrame, serializeFrame } from "../src/frame.js";
import type {
  ApplicationEventFrame,
  InitFrame,
  MediaFrame,
} from "../src/types.js";

describe("frame parser and serializer", () => {
  const initBody =
    '{"version":"0.3","streams":[{"content_type":"audio/opus","type":"output","offset_type":"message"}]}';

  it("round-trips control frames and ignores unknown headers", () => {
    const frame: InitFrame = {
      kind: "control",
      event: "culpeo.init",
      headers: {
        event: "culpeo.init",
        authorization: "Bearer redacted",
        contentType: "application/json",
        bufferWindow: 5_000,
      },
      body: {
        version: "0.3",
        streams: [{ content_type: "audio/opus", type: "output", offset_type: "message" }],
      },
    };

    const serialized = serializeFrame(frame);
    expect(serialized.frameType).toBe("text");
    if (serialized.frameType !== "text") {
      throw new Error("Expected a text frame.");
    }
    const decorated = serialized.data.replace(
      "\r\n\r\n",
      "\r\nX-Ignored: true\r\n\r\n",
    );
    const parsed = parseFrame(decorated, "text");

    expect(parsed).toEqual(frame);
  });

  it("round-trips media frames with a binary body", () => {
    const frame: MediaFrame = {
      kind: "media",
      headers: {
        streamId: "s1",
        offset: 320,
        contentType: "audio/pcm;rate=16000;channels=1;bits=16",
        timestamp: 12_000,
      },
      body: new Uint8Array([1, 2, 3, 4]),
    };

    const serialized = serializeFrame(frame);
    expect(serialized.frameType).toBe("binary");
    const parsed = parseFrame(serialized.data, "binary");

    expect(parsed).toEqual(frame);
  });

  it("parses application events as passthrough control frames", () => {
    const frame: ApplicationEventFrame = {
      kind: "control",
      event: "x-app.transcript",
      headers: {
        event: "x-app.transcript",
        contentType: "application/json",
        streamId: "s-events",
      },
      body: { text: "hello", is_final: true },
    };

    const serialized = serializeFrame(frame);
    const parsed = parseFrame(serialized.data, "text");
    expect(parsed).toEqual(frame);
  });

  it("rejects header blocks that exceed the maximum size", () => {
    const frame = `Event: culpeo.init\r\nAuthorization: ${"a".repeat(40)}\r\nContent-Type: application/json\r\n\r\n${initBody}`;
    expect(() => parseFrame(frame, "text", { maxHeaderBlockSize: 32 })).toThrow(
      /Header block exceeds maximum size/,
    );
  });

  it("rejects frames that exceed the maximum header count", () => {
    const frame = `Event: x-app.test\r\nX-One: 1\r\nX-Two: 2\r\n\r\n{}`;
    expect(() => parseFrame(frame, "text", { maxHeaderCount: 2 })).toThrow(
      /Header count exceeds maximum/,
    );
  });

  it("rejects oversized header names", () => {
    const frame = `Event: x-app.test\r\n${"X".repeat(9)}: value\r\n\r\n{}`;
    expect(() => parseFrame(frame, "text", { maxHeaderNameLength: 8 })).toThrow(
      /Header name exceeds maximum length/,
    );
  });

  it("rejects oversized header values", () => {
    const frame = `Event: x-app.test\r\nX-Test: ${"a".repeat(9)}\r\n\r\n{}`;
    expect(() =>
      parseFrame(frame, "text", { maxHeaderValueLength: 8 }),
    ).toThrow(/Header value exceeds maximum length/);
  });

  it("rejects NUL bytes in header values", () => {
    const frame = "Event: x-app.test\r\nX-Test: hello\0world\r\n\r\n{}";
    expect(() => parseFrame(frame, "text")).toThrow(
      /Header name\/value contains forbidden character/,
    );
  });

  it("rejects duplicate reserved headers", () => {
    const frame = "Event: x-app.test\r\nEvent: x-app.other\r\n\r\n{}";
    expect(() => parseFrame(frame, "text")).toThrow(
      /Duplicate reserved header: event/,
    );
  });

  it("allows duplicate unknown headers", () => {
    const frame =
      "Event: x-app.test\r\nX-Trace: first\r\nX-Trace: second\r\n\r\n{}";
    expect(() => parseFrame(frame, "text")).not.toThrow();
  });
});
