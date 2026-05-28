import { CulpeoError } from "./errors.js";

interface OffsetState {
  nextSendOffset: number;
  expectedReceiveOffset: number;
  lastSentOffset?: number;
  lastReceivedOffset?: number;
}

function parsePositiveInteger(value: string | undefined): number | undefined {
  if (value === undefined) {
    return undefined;
  }

  const parsed = Number.parseInt(value, 10);
  if (!Number.isFinite(parsed) || parsed <= 0) {
    return undefined;
  }
  return parsed;
}

function parsePcmStepBytes(contentType: string): number | undefined {
  const [rawType, ...parameterEntries] = contentType.split(";");
  const type = rawType?.trim().toLowerCase() ?? "";
  if (type !== "audio/pcm") {
    return undefined;
  }

  const parameters = new Map<string, string>();
  for (const entry of parameterEntries) {
    const [rawKey, rawValue] = entry.split("=");
    if (rawKey !== undefined && rawValue !== undefined) {
      parameters.set(rawKey.trim().toLowerCase(), rawValue.trim());
    }
  }

  const rate = parsePositiveInteger(parameters.get("rate"));
  const channels = parsePositiveInteger(parameters.get("channels"));
  const bits = parsePositiveInteger(parameters.get("bits"));
  if (rate === undefined || channels === undefined || bits === undefined) {
    throw new CulpeoError(
      "protocol-error",
      "PCM content type must include positive integer rate, channels, and bits parameters.",
    );
  }

  return channels * (bits / 8);
}

export function computeOffsetIncrement(
  contentType: string,
  payloadLength: number,
): number {
  const pcmStepBytes = parsePcmStepBytes(contentType);
  if (pcmStepBytes === undefined) {
    return 1;
  }

  if (pcmStepBytes <= 0 || payloadLength % pcmStepBytes !== 0) {
    throw new CulpeoError(
      "protocol-error",
      "PCM payload length must align to complete samples.",
    );
  }

  return payloadLength / pcmStepBytes;
}

export class OffsetTracker {
  private readonly states = new Map<string, OffsetState>();

  public register(streamId: string, initialOffset = 0): void {
    if (!this.states.has(streamId)) {
      this.states.set(streamId, {
        nextSendOffset: initialOffset,
        expectedReceiveOffset: initialOffset,
      });
    }
  }

  public allocate(
    streamId: string,
    contentType: string,
    payloadLength: number,
  ): number {
    const state = this.mustGet(streamId);
    const offset = state.nextSendOffset;
    state.lastSentOffset = offset;
    state.nextSendOffset += computeOffsetIncrement(contentType, payloadLength);
    return offset;
  }

  public recordReceived(
    streamId: string,
    offset: number,
    increment: number,
  ): void {
    const state = this.mustGet(streamId);
    if (offset !== state.expectedReceiveOffset) {
      throw new CulpeoError(
        "protocol-error",
        "Offsets must be strictly contiguous within a stream.",
      );
    }
    state.lastReceivedOffset = offset;
    state.expectedReceiveOffset += increment;
  }

  public seedResumeOffset(streamId: string, offset: number): void {
    const state = this.mustGet(streamId);
    state.lastReceivedOffset = offset;
    state.expectedReceiveOffset = offset;
    if (state.nextSendOffset < offset) {
      state.nextSendOffset = offset;
    }
  }

  public getResumeOffset(streamId: string): number {
    const state = this.mustGet(streamId);
    return Math.max(state.lastReceivedOffset ?? 0, state.lastSentOffset ?? 0);
  }

  private mustGet(streamId: string): OffsetState {
    const state = this.states.get(streamId);
    if (state === undefined) {
      throw new CulpeoError("protocol-error", "Unknown stream identifier.");
    }
    return state;
  }
}
