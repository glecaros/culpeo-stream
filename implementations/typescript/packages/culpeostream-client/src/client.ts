/**
 * CulpeoStreamClient — browser + Node.js client for the CulpeoStream protocol.
 *
 * Uses only the standard WebSocket API; works in browsers and Node.js 22+ without
 * any Node.js-specific imports.
 *
 * Security invariants enforced here:
 *  - Bearer tokens MUST NOT appear in Error messages, console output, or thrown
 *    objects.
 *  - Session IDs are opaque secrets — never logged.
 *  - wss:// is required by default; ws:// requires explicit `allowInsecure: true`
 *    and emits a console.warn.
 *  - Nonce echo for auth-refresh is handled entirely by the core session, which
 *    stores and invalidates the nonce atomically.
 */

import {
  CulpeoClientSession,
  CulpeoError,
  parseFrame,
  serializeFrame,
} from "culpeostream";
import type {
  ApplicationEventMessage,
  CloseCode,
  ConfirmedStreamDeclaration,
  InitErrorCode,
  MediaMessage,
  ProtocolVersion,
  ResumeStreamDeclaration,
  RttMeasurement,
  SessionNotification,
  SessionSnapshot,
} from "culpeostream";

import { TypedEventEmitter } from "./events.js";
import {
  computeBackoffDelayMs,
  defaultReconnectOptions,
  shouldRetry,
} from "./reconnect.js";
import type { ReconnectOptions } from "./reconnect.js";

// ---------------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------------

/**
 * A close reason delivered to the `close` event.
 * The code is either a protocol-level CloseCode (from `culpeo.close`) or an
 * InitErrorCode (from `culpeo.init-error`).
 */
export interface ClientCloseReason {
  code: CloseCode | InitErrorCode;
  reason: string;
}

/** Typed event map for CulpeoStreamClient. */
export interface ClientEventMap {
  /** Session fully established; init-ack received. */
  connected: void;
  /**
   * A reconnect attempt has been scheduled.
   * Payload is the 1-based attempt number.
   */
  reconnecting: number;
  /** Permanently disconnected (all attempts exhausted, or intentional disconnect). */
  disconnected: void;
  /** Server sent culpeo.close, or the session was rejected with culpeo.init-error. */
  close: ClientCloseReason;
  /** Incoming media frame from the server. */
  media: MediaMessage;
  /** Incoming application-level event from the server. */
  event: ApplicationEventMessage;
  /** A non-fatal runtime error (parse error, unexpected frame type, etc.). */
  error: Error;
  /** RTT measurement from a completed ping/pong exchange. */
  rtt: RttMeasurement;
}

/** Options passed to {@link CulpeoStreamClient.connect}. */
export interface ConnectOptions {
  /**
   * Initial bearer token sent in the `Authorization` header of `culpeo.init`.
   *
   * SECURITY: This value MUST NOT appear in error messages, console output,
   * or anywhere that could leak it.
   */
  token: string;

  /** Stream declarations for this session. */
  streams: ResumeStreamDeclaration[];

  /**
   * Buffer window hint in milliseconds sent to the server.
   * The server may override this in its init-ack.
   * Default: 5000.
   */
  bufferWindowMs?: number;

  /** Protocol version to advertise in culpeo.init. Default: "0.3". */
  version?: ProtocolVersion;

  /**
   * Allow insecure `ws://` connections.
   *
   * SECURITY: Default is `false`. When `true`, a `console.warn` is emitted.
   * Only set this in development or test environments.
   */
  allowInsecure?: boolean;

  /**
   * Async callback that returns a fresh bearer token when the server issues
   * a `culpeo.auth-refresh` challenge.
   *
   * SECURITY: The token returned here MUST NOT be included in any thrown
   * Error or log message.
   */
  getToken?: () => Promise<string>;

  /** Reconnection configuration overrides (merged over package defaults). */
  reconnect?: Partial<ReconnectOptions>;
}

/** Constructor options for {@link CulpeoStreamClient}. */
export interface CulpeoStreamClientOptions {
  /**
   * Override the WebSocket constructor.
   * Useful for testing — inject a mock class.
   * Default: `globalThis.WebSocket`.
   */
  WebSocket?: WebSocketConstructor;

  /**
   * Default reconnect options, applied to every `connect()` call.
   * These are merged under any per-call `options.reconnect` values.
   */
  reconnect?: Partial<ReconnectOptions>;
}

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

type WebSocketConstructor = new (
  url: string | URL,
  protocols?: string | string[],
) => WebSocket;

interface PendingConnect {
  resolve: () => void;
  reject: (err: Error) => void;
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

/**
 * CulpeoStream browser and Node.js client.
 *
 * Responsibilities:
 *  - Open and manage a WebSocket connection.
 *  - Run a CulpeoClientSession over it (frame parsing, state machine, auth).
 *  - Automatically reconnect with exponential-backoff + full jitter.
 *  - Resume the session (per-stream offsets) on reconnect.
 *  - Expose typed events for media, application events, RTT, and lifecycle.
 */
export class CulpeoStreamClient extends TypedEventEmitter<ClientEventMap> {
  private readonly WebSocketImpl: WebSocketConstructor;
  private readonly globalReconnect: Partial<ReconnectOptions>;

  // Set once in connect(); guards against double-connect.
  private connectUrl: string | undefined;
  private connectOptions: ConnectOptions | undefined;
  private reconnectOptions: ReconnectOptions = { ...defaultReconnectOptions };

  // Mutable per-reconnect state.
  private ws: WebSocket | undefined;
  private session: CulpeoClientSession | undefined;
  private snapshot: SessionSnapshot | undefined;
  private reconnectAttempt = 0;
  private intentionalDisconnect = false;
  private pendingConnect: PendingConnect | undefined;
  private reconnectTimer: ReturnType<typeof setTimeout> | undefined;

  public constructor(options?: CulpeoStreamClientOptions) {
    super();
    this.WebSocketImpl =
      options?.WebSocket ??
      (globalThis as typeof globalThis & { WebSocket?: WebSocketConstructor })
        .WebSocket ??
      (() => {
        throw new Error("WebSocket is not available in this environment.");
      })();
    this.globalReconnect = options?.reconnect ?? {};
  }

  // ---------------------------------------------------------------------------
  // Public API
  // ---------------------------------------------------------------------------

  /**
   * Connect to a CulpeoStream server.
   *
   * Resolves when the session is fully established (init-ack received).
   * Rejects if:
   *  - The URL scheme is invalid (and `allowInsecure` is not set)
   *  - The server sends `culpeo.init-error`
   *  - All reconnect attempts are exhausted
   *
   * Can only be called once per client instance.
   */
  public connect(url: string | URL, options: ConnectOptions): Promise<void> {
    if (this.connectUrl !== undefined) {
      return Promise.reject(
        new Error(
          "Already connected. Create a new CulpeoStreamClient instance to open another connection.",
        ),
      );
    }

    const urlString = typeof url === "string" ? url : url.href;
    // Wrap synchronous URL validation errors in a rejected Promise so that
    // callers always deal with a single async error channel.
    try {
      validateUrl(urlString, options.allowInsecure);
    } catch (err) {
      return Promise.reject(err);
    }

    this.connectUrl = urlString;
    this.connectOptions = options;
    this.reconnectOptions = {
      ...defaultReconnectOptions,
      ...this.globalReconnect,
      ...options.reconnect,
    };
    this.reconnectAttempt = 0;
    this.intentionalDisconnect = false;

    return new Promise<void>((resolve, reject) => {
      this.pendingConnect = { resolve, reject };
      this.openConnection();
    });
  }

  /**
   * Gracefully close the session.
   *
   * Sends `culpeo.close` to the server (if the session is established), then
   * closes the WebSocket. No reconnect is attempted after calling this method.
   *
   * Cancels any pending reconnect timer.
   */
  public disconnect(): void {
    this.intentionalDisconnect = true;
    if (this.reconnectTimer !== undefined) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = undefined;
    }
    if (this.session?.state === "established") {
      // ws.send() is synchronous, so the frame is queued before ws.close().
      void this.session.close("normal", "Client disconnect");
    }
    this.ws?.close(1000, "Client disconnect");
    this.ws = undefined;
    this.emit("disconnected");
  }

  /**
   * Send a media frame on the specified stream.
   *
   * @throws {CulpeoError} if the session is not established.
   * @throws {CulpeoError} if the stream is not writable by the client.
   */
  public sendMedia(streamId: string, data: ArrayBuffer): void {
    if (this.session === undefined || this.session.state !== "established") {
      throw new CulpeoError(
        "protocol-error",
        "Cannot send media: session is not established.",
      );
    }
    void this.session.sendMedia(streamId, new Uint8Array(data));
  }

  /**
   * Send an application-level event.
   *
   * @throws {CulpeoError} if the session is not established.
   */
  public sendEvent(
    event: string,
    body: ApplicationEventMessage["body"],
    streamId?: string,
  ): void {
    if (this.session === undefined || this.session.state !== "established") {
      throw new CulpeoError(
        "protocol-error",
        "Cannot send event: session is not established.",
      );
    }
    void this.session.sendEvent(event, body, streamId);
  }

  /**
   * Send a ping. The RTT measurement is delivered via the `rtt` event.
   *
   * @throws {CulpeoError} if the session is not established.
   */
  public sendPing(): void {
    if (this.session === undefined || this.session.state !== "established") {
      throw new CulpeoError(
        "protocol-error",
        "Cannot ping: session is not established.",
      );
    }
    void this.session.sendPing();
  }

  /**
   * Returns the confirmed stream list after the session is established.
   * Returns an empty array before `connected` is emitted.
   */
  public get confirmedStreams(): readonly ConfirmedStreamDeclaration[] {
    if (this.session?.state !== "established") return [];
    try {
      return this.session.createSnapshot().streams;
    } catch {
      return [];
    }
  }

  // ---------------------------------------------------------------------------
  // Internal: connection lifecycle
  // ---------------------------------------------------------------------------

  private openConnection(): void {
    const url = this.connectUrl!;
    const options = this.connectOptions!;
    // Capture the snapshot for this attempt; may be undefined on first attempt.
    const resumeFrom = this.snapshot;

    const ws = new this.WebSocketImpl(url);
    ws.binaryType = "arraybuffer";
    this.ws = ws;

    const session = new CulpeoClientSession({
      streams: options.streams,
      version: options.version,
      refreshAuthToken: options.getToken,
      sendFrame: (frame) => {
        // 1 = WebSocket.OPEN
        if (ws.readyState !== 1) return;
        const serialized = serializeFrame(frame);
        ws.send(serialized.data);
      },
      onRtt: (measurement) => {
        this.emit("rtt", measurement);
      },
      onNotification: (notification: SessionNotification) => {
        this.handleNotification(notification);
      },
    });
    this.session = session;

    ws.addEventListener("open", () => {
      void (async () => {
        // On reconnects, try to get a fresh token to avoid auth failures.
        // The original token is used as the fallback; it is NEVER logged.
        let authorization = `Bearer ${options.token}`;
        if (this.reconnectAttempt > 0 && options.getToken !== undefined) {
          try {
            const freshToken = await options.getToken();
            authorization = `Bearer ${freshToken}`;
          } catch {
            // Proceed with original token on refresh failure during reconnect.
          }
        }
        await session.start({
          authorization,
          bufferWindowMs: options.bufferWindowMs ?? 5000,
          resumeFrom,
        });
      })();
    });

    ws.addEventListener("message", (event: MessageEvent) => {
      try {
        const { data } = event;
        let frame;
        if (typeof data === "string") {
          frame = parseFrame(data, "text");
        } else if (data instanceof ArrayBuffer) {
          frame = parseFrame(new Uint8Array(data), "binary");
        } else {
          // Blob or other — not expected when binaryType is 'arraybuffer'.
          return;
        }
        void session.receive(frame);
      } catch (err) {
        this.emit(
          "error",
          err instanceof Error ? err : new Error(String(err)),
        );
      }
    });

    ws.addEventListener("close", () => {
      // Try to snapshot the session state for resumption on reconnect.
      if (session.state === "established") {
        try {
          this.snapshot = session.createSnapshot();
        } catch {
          // Session was established but snapshot failed; reconnect fresh.
        }
      }
      if (this.intentionalDisconnect) return;
      this.scheduleReconnect();
    });

    // The `error` event is always followed by a `close` event on a WebSocket.
    // We handle reconnect logic in the close handler to avoid double-scheduling.
    ws.addEventListener("error", () => {
      /* handled in close */
    });
  }

  private handleNotification(notification: SessionNotification): void {
    switch (notification.type) {
      case "init-ack": {
        // Successful (re)connect — reset the attempt counter.
        this.reconnectAttempt = 0;
        const pending = this.pendingConnect;
        this.pendingConnect = undefined;
        pending?.resolve();
        this.emit("connected");
        break;
      }

      case "init-error": {
        // The server explicitly rejected the session.
        // Do NOT reconnect — the server told us to stop.
        this.intentionalDisconnect = true;
        // Close the underlying WebSocket.
        this.ws?.close(1000, notification.frame.headers.code);

        const { code, reason } = notification.frame.headers;
        // SECURITY: 'code' is an error code string, NOT the token.
        const err = new Error(`Session init failed: ${code}`);

        this.emit("close", { code, reason });

        const pending = this.pendingConnect;
        this.pendingConnect = undefined;
        if (pending !== undefined) {
          pending.reject(err);
        } else {
          // Reconnect attempt got init-error — signal as a non-fatal error.
          this.emit("error", err);
          this.emit("disconnected");
        }
        break;
      }

      case "media": {
        this.emit("media", notification.frame);
        break;
      }

      case "application-event": {
        this.emit("event", notification.frame);
        break;
      }

      case "close": {
        // Server initiated a clean close — respect it, do not reconnect.
        this.intentionalDisconnect = true;
        this.ws?.close(1000, notification.frame.headers.code);
        this.emit("close", {
          code: notification.frame.headers.code,
          reason: notification.frame.headers.reason,
        });
        break;
      }
    }
  }

  // ---------------------------------------------------------------------------
  // Internal: reconnection
  // ---------------------------------------------------------------------------

  private scheduleReconnect(): void {
    if (!shouldRetry(this.reconnectAttempt, this.reconnectOptions)) {
      const err = new Error("Max reconnect attempts exceeded.");
      const pending = this.pendingConnect;
      this.pendingConnect = undefined;
      if (pending !== undefined) {
        pending.reject(err);
      } else {
        this.emit("error", err);
      }
      this.emit("disconnected");
      return;
    }

    const delay = computeBackoffDelayMs(
      this.reconnectAttempt,
      this.reconnectOptions,
    );
    this.reconnectAttempt += 1;
    this.emit("reconnecting", this.reconnectAttempt);

    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = undefined;
      this.openConnection();
    }, delay);
  }
}

// ---------------------------------------------------------------------------
// URL validation
// ---------------------------------------------------------------------------

/**
 * Validates the WebSocket URL scheme.
 *
 * SECURITY:
 *  - `wss://` is always allowed.
 *  - `ws://` requires `allowInsecure: true` and emits a console.warn.
 *  - Any other scheme throws immediately.
 */
function validateUrl(url: string, allowInsecure?: boolean): void {
  const lower = url.toLowerCase();
  if (lower.startsWith("wss://")) {
    return;
  }
  if (lower.startsWith("ws://")) {
    if (allowInsecure === true) {
      console.warn(
        "[CulpeoStream] WARNING: Connecting over ws:// (insecure WebSocket). " +
          "This exposes auth tokens and media data in transit. " +
          "Use wss:// in all non-development environments.",
      );
      return;
    }
    throw new Error(
      "Insecure ws:// connections are disabled by default. " +
        "Set allowInsecure: true to allow (development environments only).",
    );
  }
  throw new Error(
    `Invalid WebSocket URL scheme. Expected wss:// (or ws:// with allowInsecure: true). Got: ${new URL(url).protocol}`,
  );
}
