export type JsonPrimitive = string | number | boolean | null;
export type JsonValue = JsonPrimitive | JsonObject | JsonArray;
export interface JsonObject {
  [key: string]: JsonValue;
}
export interface JsonArray extends Array<JsonValue> {}

export type EndpointRole = "client" | "server";
export type SessionState =
  | "uninitialized"
  | "initializing"
  | "established"
  | "closed";
export type StreamDirection = "input" | "output" | "duplex";
export type OffsetType = "time" | "byte" | "message";
export type ControlFrameType = "text";
export type MediaFrameType = "binary";
export type TransportFrameType = ControlFrameType | MediaFrameType;
export type ProtocolVersion = "0.3" | (string & {});

export type InitErrorCode =
  | "unauthorized"
  | "unsupported-version"
  | "invalid-session"
  | "invalid-streams"
  | "protocol-error"
  | "server-error";

export type CloseCode =
  | "normal"
  | "auth-expired"
  | "server-shutdown"
  | "idle-timeout"
  | "protocol-error"
  | "unauthorized"
  | "rate-limit-exceeded";

export type KnownProtocolEvent =
  | "culpeo.init"
  | "culpeo.init-ack"
  | "culpeo.init-error"
  | "culpeo.ping"
  | "culpeo.pong"
  | "culpeo.auth-refresh"
  | "culpeo.auth-response"
  | "culpeo.close";

export interface StreamDeclaration {
  content_type: string;
  type: StreamDirection;
  offset_type: OffsetType;
  purpose?: string;
}

export interface ResumeStreamDeclaration extends StreamDeclaration {
  id?: string;
  resume_offset?: number;
}

export interface ConfirmedStreamDeclaration extends StreamDeclaration {
  id: string;
  resume_offset?: number;
}

export interface InitBody {
  version: ProtocolVersion;
  streams: ResumeStreamDeclaration[];
}

export interface InitAckBody {
  version: ProtocolVersion;
  streams: ConfirmedStreamDeclaration[];
}

export interface UnsupportedVersionBody {
  supported_versions: string[];
}

export type InitErrorBody = JsonObject | UnsupportedVersionBody;

export interface PingBody {
  ts: number;
}

export interface PongBody {
  ts: number;
  server_ts: number;
}

export interface AuthRefreshBody {
  nonce: string;
}

export interface AuthResponseBody {
  nonce: string;
}

export interface EmptyBody {}

export interface InitHeaders {
  event: "culpeo.init";
  authorization: string;
  contentType: "application/json";
  bufferWindow?: number;
  sessionId?: string;
}

export interface InitAckHeaders {
  event: "culpeo.init-ack";
  sessionId: string;
  contentType: "application/json";
  bufferWindow?: number;
}

export interface InitErrorHeaders {
  event: "culpeo.init-error";
  code: InitErrorCode;
  reason: string;
}

export interface PingHeaders {
  event: "culpeo.ping";
  contentType: "application/json";
}

export interface PongHeaders {
  event: "culpeo.pong";
  contentType: "application/json";
}

export interface AuthRefreshHeaders {
  event: "culpeo.auth-refresh";
  contentType: "application/json";
}

export interface AuthResponseHeaders {
  event: "culpeo.auth-response";
  authorization: string;
  contentType: "application/json";
}

export interface CloseHeaders {
  event: "culpeo.close";
  code: CloseCode;
  reason: string;
}

export interface ApplicationEventHeaders {
  event: string;
  contentType?: string;
  streamId?: string;
}

export interface MediaHeaders {
  streamId: string;
  offset: number;
  contentType: string;
  timestamp?: number;
}

export interface InitFrame {
  kind: "control";
  event: "culpeo.init";
  headers: InitHeaders;
  body: InitBody;
}

export interface InitAckFrame {
  kind: "control";
  event: "culpeo.init-ack";
  headers: InitAckHeaders;
  body: InitAckBody;
}

export interface InitErrorFrame {
  kind: "control";
  event: "culpeo.init-error";
  headers: InitErrorHeaders;
  body: InitErrorBody;
}

export interface PingFrame {
  kind: "control";
  event: "culpeo.ping";
  headers: PingHeaders;
  body: PingBody;
}

export interface PongFrame {
  kind: "control";
  event: "culpeo.pong";
  headers: PongHeaders;
  body: PongBody;
}

export interface AuthRefreshFrame {
  kind: "control";
  event: "culpeo.auth-refresh";
  headers: AuthRefreshHeaders;
  body: AuthRefreshBody;
}

export interface AuthResponseFrame {
  kind: "control";
  event: "culpeo.auth-response";
  headers: AuthResponseHeaders;
  body: AuthResponseBody;
}

export interface CloseFrame {
  kind: "control";
  event: "culpeo.close";
  headers: CloseHeaders;
  body: EmptyBody;
}

export interface ApplicationEventFrame {
  kind: "control";
  event: string;
  headers: ApplicationEventHeaders;
  body: JsonObject;
}

export interface MediaFrame {
  kind: "media";
  headers: MediaHeaders;
  body: Uint8Array;
}

export type ControlFrame =
  | InitFrame
  | InitAckFrame
  | InitErrorFrame
  | PingFrame
  | PongFrame
  | AuthRefreshFrame
  | AuthResponseFrame
  | CloseFrame
  | ApplicationEventFrame;

export type CulpeoFrame = ControlFrame | MediaFrame;

export interface SerializedTextFrame {
  frameType: "text";
  data: string;
}

export interface SerializedBinaryFrame {
  frameType: "binary";
  data: Uint8Array;
}

export type SerializedFrame = SerializedTextFrame | SerializedBinaryFrame;

export interface SessionSnapshot {
  version: ProtocolVersion;
  sessionId: string;
  bufferWindowMs: number;
  disconnectedAtMs?: number;
  streams: ConfirmedStreamDeclaration[];
}

export interface RttMeasurement {
  ts: number;
  serverTs: number;
  rttMicros: number;
}

export type SessionNotification =
  | { type: "init-ack"; frame: InitAckFrame }
  | { type: "init-error"; frame: InitErrorFrame }
  | { type: "media"; frame: MediaFrame }
  | { type: "application-event"; frame: ApplicationEventFrame }
  | { type: "close"; frame: CloseFrame };

export type SendFrame = (frame: CulpeoFrame) => void | Promise<void>;

export interface ClientSessionOptions {
  streams: ResumeStreamDeclaration[];
  version?: ProtocolVersion;
  sendFrame?: SendFrame;
  nowMicros?: () => number;
  onRtt?: (measurement: RttMeasurement) => void;
  onNotification?: (notification: SessionNotification) => void;
  refreshAuthToken?: () => Promise<string>;
}

export interface ServerSessionOptions {
  supportedVersions?: ProtocolVersion[];
  sendFrame?: SendFrame;
  nowMicros?: () => number;
  onRtt?: (measurement: RttMeasurement) => void;
  onNotification?: (notification: SessionNotification) => void;
  generateId?: () => string;
  resumeSnapshot?: SessionSnapshot;
  disconnectedAtMs?: number;
  maxBufferWindowMs?: number;
  authChallengeTimeoutMs?: number;
}
