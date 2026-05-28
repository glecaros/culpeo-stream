import { CulpeoError } from "./errors.js";
import type {
  ApplicationEventFrame,
  AuthRefreshFrame,
  AuthResponseFrame,
  CloseFrame,
  ControlFrame,
  CulpeoFrame,
  InitAckBody,
  InitAckFrame,
  InitBody,
  InitErrorFrame,
  InitFrame,
  JsonObject,
  MediaFrame,
  PingFrame,
  PongFrame,
  SerializedBinaryFrame,
  SerializedFrame,
  SerializedTextFrame,
  TransportFrameType,
} from "./types.js";

const encoder = new TextEncoder();
const decoder = new TextDecoder();
const forbiddenHeaderCharacters = /[\r\n\0]/;
const reservedHeaders = new Set([
  "event",
  "content-type",
  "authorization",
  "session-id",
  "stream-id",
  "offset",
  "timestamp",
  "buffer-window",
  "reason",
  "code",
]);

export interface ParseLimits {
  maxHeaderBlockSize: number;
  maxHeaderCount: number;
  maxHeaderNameLength: number;
  maxHeaderValueLength: number;
}

export const defaultParseLimits: Readonly<ParseLimits> = {
  maxHeaderBlockSize: 8192,
  maxHeaderCount: 64,
  maxHeaderNameLength: 256,
  maxHeaderValueLength: 4096,
};

function findHeaderBoundary(
  bytes: Uint8Array,
  maxHeaderBlockSize: number,
): number {
  const searchLength = Math.min(bytes.length, maxHeaderBlockSize + 4);
  for (let index = 0; index <= searchLength - 4; index += 1) {
    if (
      bytes[index] === 13 &&
      bytes[index + 1] === 10 &&
      bytes[index + 2] === 13 &&
      bytes[index + 3] === 10
    ) {
      return index;
    }
  }
  if (bytes.length > maxHeaderBlockSize) {
    throw new CulpeoError(
      "protocol-error",
      "Header block exceeds maximum size",
    );
  }
  throw new CulpeoError(
    "protocol-error",
    "Frame headers must terminate with CRLF CRLF",
  );
}

function toUint8Array(input: string | Uint8Array): Uint8Array {
  return typeof input === "string" ? encoder.encode(input) : input;
}

function parseHeaderLines(
  headerText: string,
  limits: ParseLimits,
): Map<string, string> {
  const headers = new Map<string, string>();
  const lines = headerText.length === 0 ? [] : headerText.split("\r\n");
  const seenReservedHeaders = new Set<string>();
  let lineCount = 0;
  for (const line of lines) {
    lineCount += 1;
    if (lineCount > limits.maxHeaderCount) {
      throw new CulpeoError("protocol-error", "Header count exceeds maximum");
    }

    const separator = line.indexOf(":");
    if (separator <= 0) {
      throw new CulpeoError("protocol-error", "Malformed header line.");
    }
    const name = line.slice(0, separator).trim().toLowerCase();
    const value = line.slice(separator + 1).trim();

    if (name.length > limits.maxHeaderNameLength) {
      throw new CulpeoError(
        "protocol-error",
        "Header name exceeds maximum length",
      );
    }
    if (value.length > limits.maxHeaderValueLength) {
      throw new CulpeoError(
        "protocol-error",
        "Header value exceeds maximum length",
      );
    }
    if (
      forbiddenHeaderCharacters.test(name) ||
      forbiddenHeaderCharacters.test(value)
    ) {
      throw new CulpeoError(
        "protocol-error",
        "Header name/value contains forbidden character",
      );
    }
    if (reservedHeaders.has(name)) {
      if (seenReservedHeaders.has(name)) {
        throw new CulpeoError(
          "protocol-error",
          `Duplicate reserved header: ${name}`,
        );
      }
      seenReservedHeaders.add(name);
    }

    headers.set(name, value);
  }
  return headers;
}

function parseNumber(
  headers: Map<string, string>,
  name: string,
): number | undefined {
  const value = headers.get(name);
  if (value === undefined) {
    return undefined;
  }
  const parsed = Number.parseInt(value, 10);
  if (!Number.isFinite(parsed)) {
    throw new CulpeoError(
      "protocol-error",
      `Header ${name} must be an integer.`,
    );
  }
  return parsed;
}

function isJsonObject(value: unknown): value is JsonObject {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function parseJsonObject(bytes: Uint8Array): JsonObject {
  const bodyText = bytes.length === 0 ? "{}" : decoder.decode(bytes);
  const parsed: unknown = JSON.parse(bodyText);
  if (!isJsonObject(parsed)) {
    throw new CulpeoError(
      "protocol-error",
      "Control frame bodies must be JSON objects.",
    );
  }
  return parsed;
}

function frameToHeaders(frame: CulpeoFrame): [string, string][] {
  if (frame.kind === "media") {
    return [
      ["Stream-Id", frame.headers.streamId],
      ["Offset", String(frame.headers.offset)],
      ["Content-Type", frame.headers.contentType],
      ...(frame.headers.timestamp !== undefined
        ? ([["Timestamp", String(frame.headers.timestamp)]] as [
            string,
            string,
          ][])
        : []),
    ];
  }

  return controlFrameToHeaders(frame);
}

function controlFrameToHeaders(frame: ControlFrame): [string, string][] {
  if (frame.event === "culpeo.init") {
    const initFrame = frame as InitFrame;
    return [
      ["Event", initFrame.event],
      ["Authorization", initFrame.headers.authorization],
      ["Content-Type", initFrame.headers.contentType],
      ...(initFrame.headers.sessionId !== undefined
        ? ([["Session-Id", initFrame.headers.sessionId]] as [string, string][])
        : []),
      ...(initFrame.headers.bufferWindow !== undefined
        ? ([["Buffer-Window", String(initFrame.headers.bufferWindow)]] as [
            string,
            string,
          ][])
        : []),
    ];
  }

  if (frame.event === "culpeo.init-ack") {
    const ackFrame = frame as InitAckFrame;
    return [
      ["Event", ackFrame.event],
      ["Session-Id", ackFrame.headers.sessionId],
      ["Content-Type", ackFrame.headers.contentType],
      ...(ackFrame.headers.bufferWindow !== undefined
        ? ([["Buffer-Window", String(ackFrame.headers.bufferWindow)]] as [
            string,
            string,
          ][])
        : []),
    ];
  }

  if (frame.event === "culpeo.init-error") {
    const errorFrame = frame as InitErrorFrame;
    return [
      ["Event", errorFrame.event],
      ["Code", errorFrame.headers.code],
      ["Reason", errorFrame.headers.reason],
    ];
  }

  if (
    frame.event === "culpeo.ping" ||
    frame.event === "culpeo.pong" ||
    frame.event === "culpeo.auth-refresh"
  ) {
    const jsonFrame = frame as PingFrame | PongFrame | AuthRefreshFrame;
    return [
      ["Event", jsonFrame.event],
      ["Content-Type", jsonFrame.headers.contentType],
    ];
  }

  if (frame.event === "culpeo.auth-response") {
    const responseFrame = frame as AuthResponseFrame;
    return [
      ["Event", responseFrame.event],
      ["Authorization", responseFrame.headers.authorization],
      ["Content-Type", responseFrame.headers.contentType],
    ];
  }

  if (frame.event === "culpeo.close") {
    const closeFrame = frame as CloseFrame;
    return [
      ["Event", closeFrame.event],
      ["Code", closeFrame.headers.code],
      ["Reason", closeFrame.headers.reason],
    ];
  }

  const applicationFrame = frame as ApplicationEventFrame;
  return [
    ["Event", applicationFrame.event],
    ...(applicationFrame.headers.streamId !== undefined
      ? ([["Stream-Id", applicationFrame.headers.streamId]] as [
          string,
          string,
        ][])
      : []),
    ...(applicationFrame.headers.contentType !== undefined
      ? ([["Content-Type", applicationFrame.headers.contentType]] as [
          string,
          string,
        ][])
      : []),
  ];
}

function serializeTextFrame(
  frame: Exclude<CulpeoFrame, MediaFrame>,
): SerializedTextFrame {
  const headers = frameToHeaders(frame);
  return {
    frameType: "text",
    data: `${headers.map(([name, value]) => `${name}: ${value}\r\n`).join("")}\r\n${JSON.stringify(frame.body)}`,
  };
}

function serializeBinaryFrame(frame: MediaFrame): SerializedBinaryFrame {
  const headers = `${frameToHeaders(frame)
    .map(([name, value]) => `${name}: ${value}\r\n`)
    .join("")}\r\n`;
  const headerBytes = encoder.encode(headers);
  const combined = new Uint8Array(headerBytes.length + frame.body.length);
  combined.set(headerBytes, 0);
  combined.set(frame.body, headerBytes.length);
  return { frameType: "binary", data: combined };
}

export function serializeFrame(frame: CulpeoFrame): SerializedFrame {
  return frame.kind === "media"
    ? serializeBinaryFrame(frame)
    : serializeTextFrame(frame);
}

function parseControlFrame(
  headers: Map<string, string>,
  body: JsonObject,
): CulpeoFrame {
  const event = headers.get("event");
  if (event === undefined) {
    throw new CulpeoError(
      "protocol-error",
      "Control frames require an Event header.",
    );
  }

  switch (event) {
    case "culpeo.init": {
      const frame: InitFrame = {
        kind: "control",
        event,
        headers: {
          event,
          authorization: headers.get("authorization") ?? "",
          contentType: "application/json",
          ...(headers.has("buffer-window")
            ? { bufferWindow: parseNumber(headers, "buffer-window") }
            : {}),
          ...(headers.has("session-id")
            ? { sessionId: headers.get("session-id") ?? "" }
            : {}),
        },
        body: body as unknown as InitBody,
      };
      return frame;
    }
    case "culpeo.init-ack": {
      const sessionId = headers.get("session-id");
      if (sessionId === undefined) {
        throw new CulpeoError(
          "protocol-error",
          "culpeo.init-ack requires Session-Id.",
        );
      }
      const frame: InitAckFrame = {
        kind: "control",
        event,
        headers: {
          event,
          sessionId,
          contentType: "application/json",
          ...(headers.has("buffer-window")
            ? { bufferWindow: parseNumber(headers, "buffer-window") }
            : {}),
        },
        body: body as unknown as InitAckBody,
      };
      return frame;
    }
    case "culpeo.init-error": {
      const code = headers.get("code");
      const reason = headers.get("reason");
      if (code === undefined || reason === undefined) {
        throw new CulpeoError(
          "protocol-error",
          "culpeo.init-error requires Code and Reason.",
        );
      }
      const frame: InitErrorFrame = {
        kind: "control",
        event,
        headers: {
          event,
          code: code as InitErrorFrame["headers"]["code"],
          reason,
        },
        body,
      };
      return frame;
    }
    case "culpeo.ping": {
      const frame: PingFrame = {
        kind: "control",
        event,
        headers: { event, contentType: "application/json" },
        body: body as unknown as PingFrame["body"],
      };
      return frame;
    }
    case "culpeo.pong": {
      const frame: PongFrame = {
        kind: "control",
        event,
        headers: { event, contentType: "application/json" },
        body: body as unknown as PongFrame["body"],
      };
      return frame;
    }
    case "culpeo.auth-refresh": {
      const frame: AuthRefreshFrame = {
        kind: "control",
        event,
        headers: { event, contentType: "application/json" },
        body: body as unknown as AuthRefreshFrame["body"],
      };
      return frame;
    }
    case "culpeo.auth-response": {
      const authorization = headers.get("authorization") ?? "";
      const frame: AuthResponseFrame = {
        kind: "control",
        event,
        headers: { event, authorization, contentType: "application/json" },
        body: body as unknown as AuthResponseFrame["body"],
      };
      return frame;
    }
    case "culpeo.close": {
      const code = headers.get("code");
      const reason = headers.get("reason");
      if (code === undefined || reason === undefined) {
        throw new CulpeoError(
          "protocol-error",
          "culpeo.close requires Code and Reason.",
        );
      }
      const frame: CloseFrame = {
        kind: "control",
        event,
        headers: { event, code: code as CloseFrame["headers"]["code"], reason },
        body,
      };
      return frame;
    }
    default: {
      const frame: ApplicationEventFrame = {
        kind: "control",
        event,
        headers: {
          event,
          ...(headers.has("content-type")
            ? { contentType: headers.get("content-type") ?? "" }
            : {}),
          ...(headers.has("stream-id")
            ? { streamId: headers.get("stream-id") ?? "" }
            : {}),
        },
        body,
      };
      return frame;
    }
  }
}

export function parseFrame(
  input: string | Uint8Array,
  frameType: TransportFrameType,
  limits?: Partial<ParseLimits>,
): CulpeoFrame {
  const bytes = toUint8Array(input);
  const parseLimits: ParseLimits = { ...defaultParseLimits, ...limits };
  const headerBoundary = findHeaderBoundary(
    bytes,
    parseLimits.maxHeaderBlockSize,
  );
  const headerText = decoder.decode(bytes.subarray(0, headerBoundary));
  const headers = parseHeaderLines(headerText, parseLimits);
  const bodyBytes = bytes.subarray(headerBoundary + 4);

  if (frameType === "binary") {
    const streamId = headers.get("stream-id");
    const offset = parseNumber(headers, "offset");
    const contentType = headers.get("content-type");
    if (
      streamId === undefined ||
      offset === undefined ||
      contentType === undefined
    ) {
      throw new CulpeoError(
        "protocol-error",
        "Media frames require Stream-Id, Offset, and Content-Type headers.",
      );
    }
    const frame: MediaFrame = {
      kind: "media",
      headers: {
        streamId,
        offset,
        contentType,
        ...(headers.has("timestamp")
          ? { timestamp: parseNumber(headers, "timestamp") }
          : {}),
      },
      body: bodyBytes.slice(),
    };
    return frame;
  }

  return parseControlFrame(headers, parseJsonObject(bodyBytes));
}
