/**
 * CulpeoStream server — Node.js WebSocket server using the `ws` library.
 *
 * createCulpeoServer(options) → CulpeoServer
 *   .attach(httpServer) — reuse an existing http/https server
 *   .listen(port, host?) — create a standalone server
 *   .close()            — graceful shutdown
 *
 * Security invariants enforced here:
 *  - Authentication tokens are never logged or included in Error objects.
 *  - Session IDs are never logged.
 *  - Nonces for auth-refresh are managed entirely by the core CulpeoServerSession.
 *  - The authenticate() callback result is the sole gate on session establishment.
 */

import * as http from "node:http";
import * as https from "node:https";

import { WebSocketServer } from "ws";
import type WebSocketType from "ws";

import { WebSocketClient } from "@culpeo/async-ws";

import {
  CulpeoError,
  CulpeoServerSession,
  parseFrame,
  serializeFrame,
} from "culpeostream";
import type {
  ConfirmedStreamDeclaration,
  InitErrorCode,
  InitFrame,
  JsonObject,
  SessionNotification,
  SessionSnapshot,
  StreamDeclaration,
} from "culpeostream";

import type { ICulpeoStreamHandler, IServerSession } from "./handler.js";
import type { ISessionStore } from "./store.js";
import { InMemorySessionStore } from "./store.js";

// ---------------------------------------------------------------------------
// Public options
// ---------------------------------------------------------------------------

export interface CulpeoServerOptions {
  /**
   * Authenticate an incoming session.
   * Receives the full Authorization header value (e.g. "Bearer <token>") and,
   * when the client is resuming an existing session, the session ID.
   *
   * SECURITY: The authorization value MUST NOT appear in logs or errors.
   * Implementations SHOULD verify session ownership when sessionId is provided —
   * i.e. confirm that the supplied token originally created (or is authorised to
   * resume) the identified session. Failing to do so allows any authenticated
   * user to hijack an existing session by guessing its ID (SEC-020).
   *
   * Return false to reject the connection with culpeo.init-error(unauthorized).
   */
  authenticate: (authorization: string, sessionId?: string) => Promise<boolean>;

  /** Application-level handler for session lifecycle and frame events. */
  handler: ICulpeoStreamHandler;

  /**
   * Session persistence store.
   * Default: a new InMemorySessionStore with default settings.
   */
  sessionStore?: ISessionStore;

  /**
   * Interval (ms) at which the server sends culpeo.ping to each established session.
   * Default: 30_000 (30 s). Set to 0 to disable.
   */
  pingIntervalMs?: number;

  /**
   * Interval (ms) at which the server issues culpeo.auth-refresh challenges.
   * Default: 0 (disabled).
   */
  authRefreshIntervalMs?: number;

  /**
   * Minimum interval (ms) between consecutive culpeo.auth-refresh challenges
   * for the same session.  Calls within the cooldown window are silently dropped.
   * Default: 30_000 (30 s). Set to 0 to disable rate-limiting.
   */
  minAuthRefreshIntervalMs?: number;

  /**
   * Maximum allowed WebSocket message size in bytes.
   * Messages exceeding this limit cause ws to terminate the connection (code 1009).
   * Default: 1_048_576 (1 MiB).
   */
  maxMessageBytes?: number;

  /**
   * Allow insecure ws:// connections.
   * When false (default), only wss:// clients are accepted.
   * Has no practical effect when the server is attached to a plain http.Server —
   * set this to true only in development / test environments.
   */
  allowInsecure?: boolean;
}

// ---------------------------------------------------------------------------
// CulpeoServer
// ---------------------------------------------------------------------------

export class CulpeoServer {
  private readonly options: CulpeoServerOptions;
  private readonly store: ISessionStore;
  private wss: WebSocketServer | undefined;
  private ownedHttpServer: http.Server | undefined;
  private readonly connections = new Set<ServerConnection>();

  public constructor(options: CulpeoServerOptions) {
    this.options = options;
    this.store = options.sessionStore ?? new InMemorySessionStore();
  }

  /**
   * Attach the CulpeoStream WebSocket server to an existing HTTP or HTTPS server.
   * Call this before `server.listen()`.
   */
  public attach(server: http.Server | https.Server): void {
    if (this.wss !== undefined) {
      throw new Error("CulpeoServer is already attached to a server.");
    }
    this.wss = new WebSocketServer({
      server,
      maxPayload: this.options.maxMessageBytes ?? 1_048_576,
    });
    this.setupWebSocketServer(this.wss);
  }

  /**
   * Create a standalone HTTP server and listen on the given port.
   * Resolves once the server is listening.
   */
  public listen(port: number, host?: string): Promise<void> {
    if (this.wss !== undefined) {
      return Promise.reject(
        new Error("CulpeoServer is already listening / attached."),
      );
    }
    const httpServer = http.createServer();
    this.ownedHttpServer = httpServer;
    this.wss = new WebSocketServer({
      server: httpServer,
      maxPayload: this.options.maxMessageBytes ?? 1_048_576,
    });
    this.setupWebSocketServer(this.wss);

    return new Promise((resolve, reject) => {
      httpServer.once("error", reject);
      httpServer.listen(port, host, () => {
        httpServer.removeListener("error", reject);
        resolve();
      });
    });
  }

  /**
   * Gracefully close the server:
   *  1. Terminate all active WebSocket connections.
   *  2. Close the WebSocketServer (stops accepting new connections).
   *  3. If a standalone server was created, close it too.
   */
  public close(): Promise<void> {
    return new Promise((resolve, reject) => {
      // Terminate all active connections first.
      for (const conn of this.connections) {
        conn.terminate();
      }
      this.connections.clear();

      const wss = this.wss;
      if (wss === undefined) {
        resolve();
        return;
      }
      this.wss = undefined;

      wss.close((wssErr) => {
        if (wssErr) {
          reject(wssErr);
          return;
        }
        const owned = this.ownedHttpServer;
        if (owned !== undefined) {
          this.ownedHttpServer = undefined;
          owned.close((httpErr) => {
            if (httpErr) reject(httpErr);
            else resolve();
          });
        } else {
          resolve();
        }
      });
    });
  }

  private setupWebSocketServer(wss: WebSocketServer): void {
    wss.on("connection", (ws) => {
      const conn = new ServerConnection(ws, this.options, this.store, () => {
        this.connections.delete(conn);
      });
      this.connections.add(conn);
    });
  }
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

export function createCulpeoServer(options: CulpeoServerOptions): CulpeoServer {
  return new CulpeoServer(options);
}

// ---------------------------------------------------------------------------
// ServerSessionImpl — IServerSession wrapper around CulpeoServerSession
// ---------------------------------------------------------------------------

class ServerSessionImpl implements IServerSession {
  private readonly _streams: ReadonlyMap<string, StreamDeclaration>;
  /** Epoch-ms of the last culpeo.auth-refresh challenge sent from this session. */
  private lastAuthRefreshAt = 0;

  public constructor(
    private readonly coreSession: CulpeoServerSession,
    private readonly wsClient: WebSocketClient,
    confirmedStreams: readonly ConfirmedStreamDeclaration[],
    private readonly options: CulpeoServerOptions,
  ) {
    const map = new Map<string, StreamDeclaration>();
    for (const stream of confirmedStreams) {
      const decl: StreamDeclaration = {
        type: stream.type,
        content_type: stream.content_type,
        offset_type: stream.offset_type,
        ...(stream.purpose !== undefined ? { purpose: stream.purpose } : {}),
      };
      map.set(stream.id, decl);
    }
    this._streams = map;
  }

  public get sessionId(): string {
    // Safe: sessionId is always set after init-ack is received.
    return this.coreSession.sessionId ?? "";
  }

  public get streams(): ReadonlyMap<string, StreamDeclaration> {
    return this._streams;
  }

  public sendMedia(streamId: string, data: Uint8Array): Promise<void> {
    return this.coreSession.sendMedia(streamId, data);
  }

  public sendEvent(eventName: string, body: JsonObject): Promise<void> {
    return this.coreSession.sendEvent(eventName, body);
  }

  public async requestAuthRefresh(): Promise<void> {
    const now = Date.now();
    const minInterval = this.options.minAuthRefreshIntervalMs ?? 30_000;
    if (minInterval > 0 && now - this.lastAuthRefreshAt < minInterval) {
      // Silently drop — we're within the cooldown window (SEC-021).
      return;
    }
    this.lastAuthRefreshAt = now;
    await this.coreSession.requestAuthRefresh();
  }

  public async close(reason = "Session closed by server"): Promise<void> {
    await this.coreSession.close("normal", reason);
    await this.wsClient.close(1000, reason);
  }
}

// ---------------------------------------------------------------------------
// ServerConnection — manages one WebSocket connection lifecycle
// ---------------------------------------------------------------------------

/**
 * Build a serialized culpeo.init-error frame as a string (control frame).
 * Used to reject connections before a CulpeoServerSession is created.
 *
 * SECURITY: reason must not contain auth tokens.
 */
function makeInitErrorText(code: InitErrorCode, reason: string): string {
  const serialized = serializeFrame({
    kind: "control",
    event: "culpeo.init-error",
    headers: { event: "culpeo.init-error", code, reason },
    body: {},
  });
  if (serialized.frameType !== "text") {
    throw new CulpeoError("server-error", "Unexpected binary init-error frame");
  }
  return serialized.data;
}

class ServerConnection {
  private initialized = false;
  private coreSession: CulpeoServerSession | undefined;
  private serverSession: ServerSessionImpl | undefined;
  private pingTimer: ReturnType<typeof setInterval> | undefined;
  private authRefreshTimer: ReturnType<typeof setInterval> | undefined;
  private readonly wsClient: WebSocketClient;
  private closed = false;

  public constructor(
    private readonly ws: WebSocketType,
    private readonly options: CulpeoServerOptions,
    private readonly store: ISessionStore,
    private readonly onClose: () => void,
  ) {
    this.wsClient = WebSocketClient.fromSocket(ws);
    void this.runMessageLoop();
  }

  /** Terminate the underlying WebSocket immediately (for server shutdown). */
  public terminate(): void {
    this.ws.terminate();
  }

  // ---------------------------------------------------------------------------
  // Message loop — serialises all message processing via for-await-of
  // ---------------------------------------------------------------------------

  private async runMessageLoop(): Promise<void> {
    try {
      for await (const msg of this.wsClient) {
        try {
          const frameInput: string | Uint8Array = msg.binary
            ? new Uint8Array(msg.data as ArrayBuffer)
            : (msg.data as string);
          const frame = parseFrame(frameInput, msg.binary ? "binary" : "text");

          if (!this.initialized) {
            this.initialized = true;
            await this.handleInitMessage(frame);
          } else if (this.coreSession !== undefined) {
            await this.coreSession.receive(frame);
          }
        } catch (err) {
          const reason =
            err instanceof Error ? err.message.slice(0, 123) : "Protocol error";
          await this.wsClient.close(4002, reason);
          break;
        }
      }
    } catch {
      // Abnormal close — handled in handleWsClose below.
    }

    await this.handleWsClose("WebSocket closed");
  }

  // ---------------------------------------------------------------------------
  // Init handling
  // ---------------------------------------------------------------------------

  private async handleInitMessage(
    frame: ReturnType<typeof parseFrame>,
  ): Promise<void> {
    // The first frame MUST be culpeo.init.
    if (frame.kind !== "control" || frame.event !== "culpeo.init") {
      this.sendText(
        makeInitErrorText("protocol-error", "First frame must be culpeo.init."),
      );
      await this.wsClient.close(4002, "Protocol error");
      return;
    }

    // TypeScript cannot fully narrow the ControlMessage union through the compound
    // guard above, so we assert the narrowed type explicitly.
    const initFrame = frame as InitFrame;

    // SECURITY: do not log the authorization value.
    let authenticated: boolean;
    try {
      authenticated = await this.options.authenticate(
        initFrame.headers.authorization,
        initFrame.headers.sessionId, // undefined for new sessions — callers SHOULD verify ownership
      );
    } catch {
      authenticated = false;
    }

    if (!authenticated) {
      // SECURITY: use a generic reason — do not reveal why auth failed.
      this.sendText(
        makeInitErrorText("unauthorized", "Authentication failed."),
      );
      await this.wsClient.close(4001, "Unauthorized");
      return;
    }

    // Load resume snapshot if a Session-Id was provided.
    let resumeSnapshot: SessionSnapshot | undefined;
    if (initFrame.headers.sessionId !== undefined) {
      const stored = await this.store.load(initFrame.headers.sessionId);
      resumeSnapshot = stored ?? undefined;
    }

    // Create the core server session.
    const coreSession = new CulpeoServerSession({
      resumeSnapshot,
      sendFrame: (f) => {
        const serialized = serializeFrame(f);
        if (this.wsClient.readyState !== "open") return;
        void this.wsClient.send(serialized.data);
      },
      onNotification: (n) => {
        void this.handleNotification(n);
      },
    });
    this.coreSession = coreSession;

    // Hand the init frame to the session state machine.
    // This will validate, send init-ack or init-error (via sendFrame), and fire
    // the notification. The init-error frame is sent before we close the ws.
    await coreSession.receive(initFrame);

    // If init failed (session closed without establishing), close the ws now
    // that the init-error frame has been dispatched.
    if (coreSession.state === "closed" && this.serverSession === undefined) {
      await this.wsClient.close(4002, "Init failed");
    }
  }

  // ---------------------------------------------------------------------------
  // Notification handler
  // ---------------------------------------------------------------------------

  private async handleNotification(n: SessionNotification): Promise<void> {
    switch (n.type) {
      case "init-ack": {
        const coreSession = this.coreSession!;
        this.serverSession = new ServerSessionImpl(
          coreSession,
          this.wsClient,
          n.frame.body.streams,
          this.options,
        );

        // Persist the session immediately so it can be resumed.
        try {
          const snapshot = coreSession.createSnapshot();
          await this.store.save(snapshot);
        } catch {
          /* non-fatal — session still works, just can't resume */
        }

        this.startTimers();

        try {
          await this.options.handler.onConnected(this.serverSession);
        } catch {
          /* handler errors must not crash the server */
        }
        break;
      }

      case "init-error": {
        // The core session will dispatch the init-error frame via sendFrame.
        // We do NOT close the WebSocket here — closing is done after session.receive()
        // returns in handleInitMessage, ensuring the frame is sent before close.
        break;
      }

      case "media": {
        const session = this.serverSession;
        if (session !== undefined) {
          try {
            await this.options.handler.onMedia(
              session,
              n.frame.headers.streamId,
              n.frame.body,
              BigInt(n.frame.headers.offset),
            );
          } catch (err) {
            if (this.options.handler.onError !== undefined) {
              void this.options.handler.onError(session, err);
            } else {
              console.warn(
                "[culpeostream-server] handler.onMedia threw (unhandled):",
                err,
              );
            }
          }
        }
        break;
      }

      case "application-event": {
        const session = this.serverSession;
        if (session !== undefined) {
          try {
            await this.options.handler.onEvent(
              session,
              n.frame.event,
              n.frame.body,
            );
          } catch (err) {
            if (this.options.handler.onError !== undefined) {
              void this.options.handler.onError(session, err);
            } else {
              console.warn(
                "[culpeostream-server] handler.onEvent threw (unhandled):",
                err,
              );
            }
          }
        }
        break;
      }

      case "close": {
        // Client sent culpeo.close — save state and close the WebSocket.
        await this.saveSnapshot();
        await this.wsClient.close(1000, n.frame.headers.reason);
        break;
      }
    }
  }

  // ---------------------------------------------------------------------------
  // WebSocket close handler
  // ---------------------------------------------------------------------------

  private async handleWsClose(reason: string): Promise<void> {
    if (this.closed) return;
    this.closed = true;

    this.stopTimers();

    // Save the final snapshot for potential session resumption.
    await this.saveSnapshot();

    const session = this.serverSession;
    if (session !== undefined) {
      try {
        await this.options.handler.onDisconnected(session, reason);
      } catch {
        /* handler errors must not crash the server */
      }
    }

    this.onClose();
  }

  // ---------------------------------------------------------------------------
  // Timers
  // ---------------------------------------------------------------------------

  private startTimers(): void {
    const pingMs = this.options.pingIntervalMs ?? 30_000;
    if (pingMs > 0) {
      this.pingTimer = setInterval(() => {
        if (this.coreSession?.state === "established") {
          void this.coreSession.sendPing();
        }
      }, pingMs);
    }

    const authMs = this.options.authRefreshIntervalMs ?? 0;
    if (authMs > 0) {
      this.authRefreshTimer = setInterval(() => {
        if (this.coreSession?.state === "established") {
          void this.coreSession.requestAuthRefresh();
        }
      }, authMs);
    }
  }

  private stopTimers(): void {
    if (this.pingTimer !== undefined) {
      clearInterval(this.pingTimer);
      this.pingTimer = undefined;
    }
    if (this.authRefreshTimer !== undefined) {
      clearInterval(this.authRefreshTimer);
      this.authRefreshTimer = undefined;
    }
  }

  // ---------------------------------------------------------------------------
  // Helpers
  // ---------------------------------------------------------------------------

  private async saveSnapshot(): Promise<void> {
    const coreSession = this.coreSession;
    if (coreSession === undefined) return;
    if (coreSession.state !== "established" && coreSession.state !== "closed") {
      return;
    }
    try {
      const snapshot = coreSession.createSnapshot();
      await this.store.save(snapshot);
    } catch {
      /* snapshot may fail if session was never established */
    }
  }

  private sendText(text: string): void {
    if (this.wsClient.readyState === "open") {
      void this.wsClient.send(text);
    }
  }
}
