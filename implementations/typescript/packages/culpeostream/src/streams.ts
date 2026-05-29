import { CulpeoError } from "./errors.js";
import { computeOffsetIncrement, OffsetTracker } from "./offsets.js";
import type {
  ConfirmedStreamDeclaration,
  EndpointRole,
  OffsetType,
  ResumeStreamDeclaration,
  SessionSnapshot,
  StreamDeclaration,
  StreamDirection,
} from "./types.js";

interface RegisteredStream extends ConfirmedStreamDeclaration {
  logicalKey: string;
}

function normalizeContentType(contentType: string): string {
  return contentType.trim().toLowerCase();
}

function logicalKey(stream: StreamDeclaration): string {
  return `${stream.type}:${stream.purpose ?? ""}:${stream.content_type}:${stream.offset_type}`;
}

function assertValidDirection(type: string): asserts type is StreamDirection {
  if (type !== "input" && type !== "output" && type !== "duplex") {
    throw new CulpeoError(
      "invalid-streams",
      "Stream types must be input, output, or duplex.",
    );
  }
}

export function validateStreamDeclarations(
  streams: readonly StreamDeclaration[],
  maxStreamCount = 16,
): void {
  if (streams.length === 0) {
    throw new CulpeoError(
      "invalid-streams",
      "At least one stream must be declared.",
    );
  }
  if (streams.length > maxStreamCount) {
    throw new CulpeoError(
      "invalid-streams",
      `Maximum of ${maxStreamCount} streams per session.`,
    );
  }

  const streamsByType = new Map<StreamDirection, StreamDeclaration[]>();
  for (const stream of streams) {
    if (stream.content_type.trim().length === 0) {
      throw new CulpeoError(
        "invalid-streams",
        "Each stream must declare a content_type.",
      );
    }
    assertValidDirection(stream.type);
    if (
      stream.offset_type !== "time" &&
      stream.offset_type !== "byte" &&
      stream.offset_type !== "message"
    ) {
      throw new CulpeoError(
        "invalid-streams",
        `Invalid or missing offset_type '${stream.offset_type}'. Must be 'time', 'byte', or 'message'.`,
      );
    }
    const group = streamsByType.get(stream.type) ?? [];
    group.push(stream);
    streamsByType.set(stream.type, group);
  }

  for (const [type, group] of streamsByType) {
    if (group.length < 2) {
      continue;
    }

    const seenPurposes = new Set<string>();
    for (const stream of group) {
      if (stream.purpose === undefined || stream.purpose.trim().length === 0) {
        throw new CulpeoError(
          "invalid-streams",
          `Streams of type ${type} require unique purpose values.`,
        );
      }
      if (seenPurposes.has(stream.purpose)) {
        throw new CulpeoError(
          "invalid-streams",
          `Duplicate purpose values are not allowed for type ${type}.`,
        );
      }
      seenPurposes.add(stream.purpose);
    }
  }
}

function allowsSender(
  streamType: StreamDirection,
  sender: EndpointRole,
): boolean {
  if (streamType === "duplex") {
    return true;
  }
  return sender === "client" ? streamType === "input" : streamType === "output";
}

export class StreamRegistry {
  private readonly plannedStreams: ResumeStreamDeclaration[];
  private readonly offsets = new OffsetTracker();
  private readonly confirmedStreams = new Map<string, RegisteredStream>();
  private confirmedOrder: RegisteredStream[] = [];

  public constructor(
    private readonly localRole: EndpointRole,
    streams: readonly ResumeStreamDeclaration[] = [],
  ) {
    if (streams.length > 0) {
      validateStreamDeclarations(streams);
    }
    this.plannedStreams = streams.map((stream) => ({ ...stream }));
  }

  public buildInitStreams(
    snapshot?: SessionSnapshot,
  ): ResumeStreamDeclaration[] {
    if (snapshot === undefined) {
      return this.plannedStreams.map((stream) => ({ ...stream }));
    }

    const available = [...snapshot.streams];
    return this.plannedStreams.map((stream) => {
      const index = available.findIndex(
        (candidate) => logicalKey(candidate) === logicalKey(stream),
      );
      const matched = index >= 0 ? available.splice(index, 1)[0] : undefined;
      if (matched === undefined) {
        return { ...stream };
      }
      return {
        ...stream,
        id: matched.id,
        resume_offset: matched.resume_offset ?? 0,
      };
    });
  }

  public confirmFromAck(streams: readonly ConfirmedStreamDeclaration[]): void {
    if (streams.length !== this.plannedStreams.length) {
      throw new CulpeoError(
        "protocol-error",
        "Server acknowledged an unexpected stream count.",
      );
    }
    this.confirm(streams);
  }

  public confirmForServer(
    streams: readonly ConfirmedStreamDeclaration[],
  ): void {
    this.confirm(streams);
  }

  public trackSend(
    streamId: string,
    payloadLength: number,
  ): { contentType: string; offset: number } {
    const stream = this.get(streamId);
    if (!allowsSender(stream.type, this.localRole)) {
      throw new CulpeoError(
        "protocol-error",
        "Media direction is invalid for this stream.",
      );
    }
    return {
      contentType: stream.content_type,
      offset: this.offsets.allocate(
        streamId,
        stream.offset_type,
        payloadLength,
        stream.content_type,
      ),
    };
  }

  public trackReceive(
    streamId: string,
    offset: number,
    contentType: string,
    payloadLength: number,
  ): void {
    const stream = this.get(streamId);
    const remoteRole: EndpointRole =
      this.localRole === "client" ? "server" : "client";
    if (!allowsSender(stream.type, remoteRole)) {
      throw new CulpeoError(
        "protocol-error",
        "Media direction is invalid for this stream.",
      );
    }
    if (
      normalizeContentType(contentType) !==
      normalizeContentType(stream.content_type)
    ) {
      throw new CulpeoError(
        "protocol-error",
        "Media frame content type does not match stream declaration.",
      );
    }
    this.offsets.recordReceived(
      streamId,
      offset,
      computeOffsetIncrement(stream.offset_type, payloadLength, stream.content_type),
    );
  }

  public has(streamId: string): boolean {
    return this.confirmedStreams.has(streamId);
  }

  public snapshot(
    sessionId: string,
    version: string,
    bufferWindowMs: number,
  ): SessionSnapshot {
    return {
      sessionId,
      version,
      bufferWindowMs,
      streams: this.confirmedOrder.map((stream) => ({
        id: stream.id,
        type: stream.type,
        content_type: stream.content_type,
        offset_type: stream.offset_type,
        ...(stream.purpose !== undefined ? { purpose: stream.purpose } : {}),
        resume_offset: this.offsets.getResumeOffset(stream.id),
      })),
    };
  }

  public getContentType(streamId: string): string {
    return this.get(streamId).content_type;
  }

  public getConfirmedStreams(): ConfirmedStreamDeclaration[] {
    return this.confirmedOrder.map((stream) => ({
      id: stream.id,
      type: stream.type,
      content_type: stream.content_type,
      offset_type: stream.offset_type,
      ...(stream.purpose !== undefined ? { purpose: stream.purpose } : {}),
      resume_offset: this.offsets.getResumeOffset(stream.id),
    }));
  }

  private confirm(streams: readonly ConfirmedStreamDeclaration[]): void {
    validateStreamDeclarations(streams);
    this.confirmedStreams.clear();
    this.confirmedOrder = streams.map((stream) => ({
      ...stream,
      logicalKey: logicalKey(stream),
    }));

    for (const stream of this.confirmedOrder) {
      this.confirmedStreams.set(stream.id, stream);
      this.offsets.register(stream.id, stream.resume_offset ?? 0);
      if (stream.resume_offset !== undefined) {
        this.offsets.seedResumeOffset(stream.id, stream.resume_offset);
      }
    }
  }

  private get(streamId: string): RegisteredStream {
    const stream = this.confirmedStreams.get(streamId);
    if (stream === undefined) {
      throw new CulpeoError("protocol-error", "Unknown stream identifier.");
    }
    return stream;
  }
}
