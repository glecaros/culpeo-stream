import { CulpeoError } from "./errors.js";
import { createRandomId } from "./random.js";
import { StreamRegistry, validateStreamDeclarations } from "./streams.js";
import type {
  ApplicationEventMessage,
  AuthRefreshFrame,
  AuthResponseFrame,
  ClientSessionOptions,
  CloseCode,
  CloseFrame,
  ConfirmedStreamDeclaration,
  CulpeoMessage,
  InitAckFrame,
  InitErrorBody,
  InitErrorCode,
  InitErrorFrame,
  InitFrame,
  PingFrame,
  PongFrame,
  ProtocolVersion,
  ResumeStreamDeclaration,
  RttMeasurement,
  ServerSessionOptions,
  SessionNotification,
  SessionSnapshot,
  SessionState,
  UnsupportedVersionBody,
} from "./types.js";

interface StartOptions {
  authorization: string;
  bufferWindowMs: number;
  resumeFrom?: SessionSnapshot;
}

function defaultNowMicros(): number {
  return Math.trunc(Date.now() * 1_000);
}

function notify(
  onNotification: ((notification: SessionNotification) => void) | undefined,
  notification: SessionNotification,
): void {
  onNotification?.(notification);
}

function ensureEstablished(state: SessionState): void {
  if (state !== "established") {
    throw new CulpeoError("protocol-error", "The session is not established.");
  }
}

function getSupportedVersions(body: InitErrorBody): string[] | undefined {
  const candidate = body as Partial<UnsupportedVersionBody>;
  return Array.isArray(candidate.supported_versions)
    ? candidate.supported_versions.slice()
    : undefined;
}

abstract class SessionBase {
  protected readonly nowMicros: () => number;
  protected readonly onRtt: ((measurement: RttMeasurement) => void) | undefined;
  protected readonly onNotification:
    | ((notification: SessionNotification) => void)
    | undefined;
  protected readonly sendFrame:
    | ((frame: CulpeoMessage) => void | Promise<void>)
    | undefined;
  protected readonly pendingPings = new Map<number, number>();
  protected readonly registry: StreamRegistry;
  protected stateValue: SessionState = "uninitialized";
  protected sessionIdValue: string | undefined;
  protected bufferWindowValue = 0;
  protected versionValue: ProtocolVersion;
  protected closeSent = false;

  protected constructor(
    role: "client" | "server",
    streams: readonly ResumeStreamDeclaration[],
    options: {
      sendFrame?: (frame: CulpeoMessage) => void | Promise<void>;
      nowMicros?: () => number;
      onRtt?: (measurement: RttMeasurement) => void;
      onNotification?: (notification: SessionNotification) => void;
      version?: ProtocolVersion;
    },
  ) {
    this.registry = new StreamRegistry(role, streams);
    this.sendFrame = options.sendFrame;
    this.nowMicros = options.nowMicros ?? defaultNowMicros;
    this.onRtt = options.onRtt;
    this.onNotification = options.onNotification;
    this.versionValue = options.version ?? "0.3";
  }

  public get state(): SessionState {
    return this.stateValue;
  }

  public get sessionId(): string | undefined {
    return this.sessionIdValue;
  }

  public get bufferWindowMs(): number {
    return this.bufferWindowValue;
  }

  public createSnapshot(): SessionSnapshot {
    if (this.sessionIdValue === undefined) {
      throw new CulpeoError(
        "protocol-error",
        "Cannot snapshot a session before it has a session identifier.",
      );
    }
    return {
      ...this.registry.snapshot(
        this.sessionIdValue,
        this.versionValue,
        this.bufferWindowValue,
      ),
      disconnectedAtMs: Math.trunc(this.nowMicros() / 1_000),
    };
  }

  public async sendMedia(
    streamId: string,
    body: Uint8Array,
    timestamp?: number,
  ): Promise<void> {
    ensureEstablished(this.stateValue);
    const tracked = this.registry.trackSend(streamId, body.length);
    await this.dispatch({
      kind: "media",
      headers: {
        streamId,
        offset: tracked.offset,
        contentType: tracked.contentType,
        ...(timestamp !== undefined ? { timestamp } : {}),
      },
      body,
    });
  }

  public async sendEvent(
    event: string,
    body: ApplicationEventMessage["body"],
    streamId?: string,
  ): Promise<void> {
    ensureEstablished(this.stateValue);
    if (streamId !== undefined && !this.registry.has(streamId)) {
      throw new CulpeoError("protocol-error", "Unknown stream identifier.");
    }
    await this.dispatch({
      kind: "control",
      event,
      headers: {
        event,
        contentType: "application/json",
        ...(streamId !== undefined ? { streamId } : {}),
      },
      body,
    });
  }

  public async sendPing(): Promise<void> {
    ensureEstablished(this.stateValue);
    const ts = this.nowMicros();
    this.pendingPings.set(ts, ts);
    await this.dispatch({
      kind: "control",
      event: "culpeo.ping",
      headers: { event: "culpeo.ping", contentType: "application/json" },
      body: { ts },
    });
  }

  public async close(code: CloseCode, reason: string): Promise<void> {
    if (this.stateValue === "closed") {
      return;
    }
    this.stateValue = "closed";
    if (!this.closeSent) {
      this.closeSent = true;
      await this.dispatch({
        kind: "control",
        event: "culpeo.close",
        headers: { event: "culpeo.close", code, reason },
        body: {},
      });
    }
  }

  protected async handlePing(frame: PingFrame): Promise<void> {
    await this.dispatch({
      kind: "control",
      event: "culpeo.pong",
      headers: { event: "culpeo.pong", contentType: "application/json" },
      body: { ts: frame.body.ts, server_ts: this.nowMicros() },
    });
  }

  protected handlePong(frame: PongFrame): void {
    const startedAt = this.pendingPings.get(frame.body.ts);
    if (startedAt === undefined) {
      return;
    }
    this.pendingPings.delete(frame.body.ts);
    this.onRtt?.({
      ts: frame.body.ts,
      serverTs: frame.body.server_ts,
      rttMicros: this.nowMicros() - startedAt,
    });
  }

  protected async handleClose(frame: CloseFrame): Promise<void> {
    notify(this.onNotification, { type: "close", frame });
    if (!this.closeSent) {
      this.closeSent = true;
      await this.dispatch({
        kind: "control",
        event: "culpeo.close",
        headers: {
          event: "culpeo.close",
          code: frame.headers.code,
          reason: frame.headers.reason,
        },
        body: {},
      });
    }
    this.stateValue = "closed";
  }

  protected handleMedia(frame: CulpeoMessage): void {
    if (frame.kind !== "media") {
      return;
    }
    this.registry.trackReceive(
      frame.headers.streamId,
      frame.headers.offset,
      frame.headers.contentType,
      frame.body.length,
    );
    notify(this.onNotification, { type: "media", frame });
  }

  protected handleApplication(frame: ApplicationEventMessage): void {
    if (
      frame.headers.streamId !== undefined &&
      !this.registry.has(frame.headers.streamId)
    ) {
      throw new CulpeoError("protocol-error", "Unknown stream identifier.");
    }
    if (!frame.event.startsWith("culpeo.")) {
      notify(this.onNotification, { type: "application-event", frame });
    }
  }

  protected async failWithClose(
    code: CloseCode,
    reason: string,
  ): Promise<never> {
    await this.close(code, reason);
    throw new CulpeoError(code, reason);
  }

  protected async dispatch(frame: CulpeoMessage): Promise<void> {
    await this.sendFrame?.(frame);
  }
}

export class CulpeoClientSession extends SessionBase {
  private readonly refreshAuthToken: (() => Promise<string>) | undefined;
  public lastInitError: InitErrorFrame | undefined;
  public supportedVersionsFromError: string[] | undefined;

  public constructor(options: ClientSessionOptions) {
    validateStreamDeclarations(options.streams);
    super("client", options.streams, options);
    this.refreshAuthToken = options.refreshAuthToken;
  }

  public async start({
    authorization,
    bufferWindowMs,
    resumeFrom,
  }: StartOptions): Promise<void> {
    if (this.stateValue !== "uninitialized") {
      throw new CulpeoError(
        "protocol-error",
        "Session start may only be called once.",
      );
    }
    const streams = this.registry.buildInitStreams(resumeFrom);
    this.bufferWindowValue = bufferWindowMs;
    this.sessionIdValue = resumeFrom?.sessionId;
    this.stateValue = "initializing";
    await this.dispatch({
      kind: "control",
      event: "culpeo.init",
      headers: {
        event: "culpeo.init",
        authorization,
        contentType: "application/json",
        bufferWindow: bufferWindowMs,
        ...(resumeFrom !== undefined
          ? { sessionId: resumeFrom.sessionId }
          : {}),
      },
      body: {
        version: this.versionValue,
        streams,
      },
    });
  }

  public async receive(frame: CulpeoMessage): Promise<void> {
    if (this.stateValue === "closed") {
      throw new CulpeoError(
        "protocol-error",
        "Closed sessions cannot receive more frames.",
      );
    }

    if (this.stateValue === "initializing") {
      if (frame.kind !== "control") {
        return this.failWithClose(
          "protocol-error",
          "Expected culpeo.init-ack or culpeo.init-error.",
        );
      }
      if (frame.event === "culpeo.init-ack") {
        this.handleInitAck(frame as InitAckFrame);
        return;
      }
      if (frame.event === "culpeo.init-error") {
        this.handleInitError(frame as InitErrorFrame);
        return;
      }
      return this.failWithClose(
        "protocol-error",
        "Expected culpeo.init-ack or culpeo.init-error.",
      );
    }

    if (this.stateValue !== "established") {
      await this.failWithClose(
        "protocol-error",
        "Client session is not ready for frames.",
      );
    }

    if (frame.kind === "media") {
      this.handleMedia(frame);
      return;
    }

    switch (frame.event) {
      case "culpeo.ping":
        await this.handlePing(frame as PingFrame);
        return;
      case "culpeo.pong":
        this.handlePong(frame as PongFrame);
        return;
      case "culpeo.auth-refresh":
        await this.handleAuthRefresh(frame as AuthRefreshFrame);
        return;
      case "culpeo.close":
        await this.handleClose(frame as CloseFrame);
        return;
      case "culpeo.init":
      case "culpeo.init-ack":
      case "culpeo.init-error":
      case "culpeo.auth-response":
        await this.failWithClose(
          "protocol-error",
          "Received a frame that is invalid in the current state.",
        );
        return;
      default:
        this.handleApplication(frame as ApplicationEventMessage);
    }
  }

  private handleInitAck(frame: InitAckFrame): void {
    this.registry.confirmFromAck(frame.body.streams);
    this.sessionIdValue = frame.headers.sessionId;
    this.bufferWindowValue =
      frame.headers.bufferWindow ?? this.bufferWindowValue;
    this.versionValue = frame.body.version;
    this.stateValue = "established";
    notify(this.onNotification, { type: "init-ack", frame });
  }

  private handleInitError(frame: InitErrorFrame): void {
    this.lastInitError = frame;
    this.supportedVersionsFromError = getSupportedVersions(frame.body);
    this.stateValue = "closed";
    notify(this.onNotification, { type: "init-error", frame });
  }

  private async handleAuthRefresh(frame: AuthRefreshFrame): Promise<void> {
    if (this.refreshAuthToken === undefined) {
      return this.failWithClose("auth-expired", "Credential refresh failed.");
    }

    let token: string;
    try {
      token = await this.refreshAuthToken();
    } catch {
      await this.failWithClose("auth-expired", "Credential refresh failed.");
      return;
    }

    await this.dispatch({
      kind: "control",
      event: "culpeo.auth-response",
      headers: {
        event: "culpeo.auth-response",
        authorization: `Bearer ${token}`,
        contentType: "application/json",
      },
      body: { nonce: frame.body.nonce },
    });
  }
}

function createInitError(
  code: InitErrorCode,
  reason: string,
  body: InitErrorBody = {},
): InitErrorFrame {
  return {
    kind: "control",
    event: "culpeo.init-error",
    headers: {
      event: "culpeo.init-error",
      code,
      reason,
    },
    body,
  };
}

function buildServerConfirmedStreams(
  requestedStreams: readonly ResumeStreamDeclaration[],
  generateId: () => string,
  resumeSnapshot?: SessionSnapshot,
): ConfirmedStreamDeclaration[] {
  if (resumeSnapshot === undefined) {
    return requestedStreams.map((stream) => ({
      id: generateId(),
      type: stream.type,
      content_type: stream.content_type,
      offset_type: stream.offset_type,
      ...(stream.purpose !== undefined ? { purpose: stream.purpose } : {}),
    }));
  }

  if (requestedStreams.length !== resumeSnapshot.streams.length) {
    throw new CulpeoError(
      "invalid-streams",
      "Resumption stream declarations do not match the existing session.",
    );
  }

  const remaining = [...resumeSnapshot.streams];
  const confirmed = requestedStreams.map((stream) => {
    const index = remaining.findIndex(
      (candidate) =>
        candidate.type === stream.type &&
        candidate.content_type === stream.content_type &&
        candidate.offset_type === stream.offset_type &&
        candidate.purpose === stream.purpose,
    );
    const matched = index >= 0 ? remaining.splice(index, 1)[0] : undefined;
    if (matched === undefined) {
      throw new CulpeoError(
        "invalid-streams",
        "Resumption stream declarations do not match the existing session.",
      );
    }
    const currentOffset = matched.resume_offset ?? 0;
    const requestedOffset = stream.resume_offset ?? currentOffset;
    if (requestedOffset > currentOffset) {
      throw new CulpeoError(
        "invalid-streams",
        "Resumption stream declarations do not match the existing session.",
      );
    }
    return {
      id: matched.id,
      type: matched.type,
      content_type: matched.content_type,
      offset_type: matched.offset_type,
      ...(matched.purpose !== undefined ? { purpose: matched.purpose } : {}),
      resume_offset: requestedOffset,
    };
  });

  if (remaining.length > 0) {
    throw new CulpeoError(
      "invalid-streams",
      "Resumption stream declarations do not match the existing session.",
    );
  }

  return confirmed;
}

export class CulpeoServerSession extends SessionBase {
  private readonly supportedVersions: readonly ProtocolVersion[];
  private readonly generateId: () => string;
  private readonly resumeSnapshot: SessionSnapshot | undefined;
  private readonly disconnectedAtMs: number | undefined;
  private readonly maxBufferWindowMs: number;
  private readonly authChallengeTimeoutMicros: number;
  private readonly receivedPingTimestampsMs: number[] = [];
  private pendingAuthNonce: string | undefined;
  private authChallengeIssuedAt: number | undefined;

  public constructor(options: ServerSessionOptions = {}) {
    super("server", [], options);
    this.supportedVersions = options.supportedVersions ?? ["0.3"];
    this.generateId = options.generateId ?? (() => createRandomId());
    this.resumeSnapshot = options.resumeSnapshot;
    this.disconnectedAtMs =
      options.disconnectedAtMs ?? options.resumeSnapshot?.disconnectedAtMs;
    this.maxBufferWindowMs = Math.max(0, options.maxBufferWindowMs ?? 30_000);
    this.authChallengeTimeoutMicros =
      Math.max(1, options.authChallengeTimeoutMs ?? 30_000) * 1_000;
  }

  public async receive(frame: CulpeoMessage): Promise<void> {
    await this.checkTimeouts();
    if (this.stateValue === "closed") {
      throw new CulpeoError(
        "protocol-error",
        "Closed sessions cannot receive more frames.",
      );
    }

    if (this.stateValue === "uninitialized") {
      if (frame.kind !== "control" || frame.event !== "culpeo.init") {
        await this.failWithClose(
          "protocol-error",
          "The first frame must be culpeo.init.",
        );
      }
      await this.handleInit(frame as InitFrame);
      return;
    }

    if (this.stateValue !== "established") {
      await this.failWithClose(
        "protocol-error",
        "Server session is not ready for frames.",
      );
    }

    if (frame.kind === "media") {
      this.handleMedia(frame);
      return;
    }

    switch (frame.event) {
      case "culpeo.ping":
        await this.handleIncomingPing(frame as PingFrame);
        return;
      case "culpeo.pong":
        this.handlePong(frame as PongFrame);
        return;
      case "culpeo.auth-response":
        await this.handleAuthResponse(frame as AuthResponseFrame);
        return;
      case "culpeo.close":
        await this.handleClose(frame as CloseFrame);
        return;
      case "culpeo.init":
      case "culpeo.init-ack":
      case "culpeo.init-error":
      case "culpeo.auth-refresh":
        await this.failWithClose(
          "protocol-error",
          "Received a frame that is invalid in the current state.",
        );
        return;
      default:
        this.handleApplication(frame as ApplicationEventMessage);
    }
  }

  public async requestAuthRefresh(): Promise<string> {
    ensureEstablished(this.stateValue);
    const nonce = this.generateId();
    this.pendingAuthNonce = nonce;
    this.authChallengeIssuedAt = this.nowMicros();
    await this.dispatch({
      kind: "control",
      event: "culpeo.auth-refresh",
      headers: {
        event: "culpeo.auth-refresh",
        contentType: "application/json",
      },
      body: { nonce },
    });
    return nonce;
  }

  public async checkTimeouts(): Promise<void> {
    if (
      this.pendingAuthNonce !== undefined &&
      this.authChallengeIssuedAt !== undefined &&
      this.nowMicros() - this.authChallengeIssuedAt >
        this.authChallengeTimeoutMicros
    ) {
      await this.failWithClose(
        "auth-expired",
        "Authentication challenge timed out.",
      );
    }
  }

  private async handleIncomingPing(frame: PingFrame): Promise<void> {
    const nowMs = Math.trunc(this.nowMicros() / 1_000);
    while (
      this.receivedPingTimestampsMs.length > 0 &&
      nowMs - this.receivedPingTimestampsMs[0]! >= 1_000
    ) {
      this.receivedPingTimestampsMs.shift();
    }
    if (this.receivedPingTimestampsMs.length >= 5) {
      return;
    }
    this.receivedPingTimestampsMs.push(nowMs);
    await this.handlePing(frame);
  }

  private async handleInit(frame: InitFrame): Promise<void> {
    this.stateValue = "initializing";

    if (frame.headers.authorization.trim().length === 0) {
      await this.sendInitErrorAndClose(
        createInitError("unauthorized", "Authorization is required."),
      );
      return;
    }

    try {
      validateStreamDeclarations(frame.body.streams);
    } catch (error) {
      if (error instanceof CulpeoError) {
        await this.sendInitErrorAndClose(
          createInitError(error.code as InitErrorCode, error.message),
        );
        return;
      }
      throw error;
    }

    if (!this.supportedVersions.includes(frame.body.version)) {
      await this.sendInitErrorAndClose(
        createInitError(
          "unsupported-version",
          "Protocol version not supported.",
          {
            supported_versions: [...this.supportedVersions],
          },
        ),
      );
      return;
    }

    const isResume = frame.headers.sessionId !== undefined;
    if (
      isResume &&
      this.resumeSnapshot?.sessionId !== frame.headers.sessionId
    ) {
      await this.sendInitErrorAndClose(
        createInitError("invalid-session", "Session cannot be resumed."),
      );
      return;
    }
    if (isResume && this.resumeSnapshot === undefined) {
      await this.sendInitErrorAndClose(
        createInitError("invalid-session", "Session cannot be resumed."),
      );
      return;
    }
    if (
      isResume &&
      this.resumeSnapshot !== undefined &&
      this.disconnectedAtMs !== undefined
    ) {
      const expiresAtMs =
        this.disconnectedAtMs + this.resumeSnapshot.bufferWindowMs;
      if (
        this.resumeSnapshot.bufferWindowMs === 0 ||
        Math.trunc(this.nowMicros() / 1_000) > expiresAtMs
      ) {
        await this.sendInitErrorAndClose(
          createInitError("invalid-session", "Session cannot be resumed."),
        );
        return;
      }
    }

    const sessionId = isResume
      ? (this.resumeSnapshot?.sessionId ?? this.generateId())
      : this.generateId();
    let confirmedStreams: ConfirmedStreamDeclaration[];
    try {
      confirmedStreams = buildServerConfirmedStreams(
        frame.body.streams,
        this.generateId,
        this.resumeSnapshot,
      );
    } catch (error) {
      if (error instanceof CulpeoError) {
        await this.sendInitErrorAndClose(
          createInitError(error.code as InitErrorCode, error.message),
        );
        return;
      }
      throw error;
    }
    this.registry.confirmForServer(confirmedStreams);
    this.sessionIdValue = sessionId;
    const requestedBufferWindow = frame.headers.bufferWindow ?? 0;
    this.bufferWindowValue =
      requestedBufferWindow === 0
        ? 0
        : Math.min(requestedBufferWindow, this.maxBufferWindowMs);
    this.versionValue = frame.body.version;
    const ack: InitAckFrame = {
      kind: "control",
      event: "culpeo.init-ack",
      headers: {
        event: "culpeo.init-ack",
        sessionId,
        contentType: "application/json",
        bufferWindow: this.bufferWindowValue,
      },
      body: {
        version: this.versionValue,
        streams: confirmedStreams,
      },
    };
    await this.dispatch(ack);
    this.stateValue = "established";
    notify(this.onNotification, { type: "init-ack", frame: ack });
  }

  private async sendInitErrorAndClose(frame: InitErrorFrame): Promise<void> {
    notify(this.onNotification, { type: "init-error", frame });
    await this.dispatch(frame);
    this.stateValue = "closed";
  }

  private async handleAuthResponse(frame: AuthResponseFrame): Promise<void> {
    if (this.pendingAuthNonce === undefined) {
      await this.failWithClose(
        "unauthorized",
        "Authentication challenge failed.",
      );
    }
    if (
      frame.body.nonce !== this.pendingAuthNonce ||
      frame.headers.authorization.trim().length === 0
    ) {
      await this.failWithClose(
        "unauthorized",
        "Authentication challenge failed.",
      );
    }
    this.pendingAuthNonce = undefined;
    this.authChallengeIssuedAt = undefined;
  }
}
