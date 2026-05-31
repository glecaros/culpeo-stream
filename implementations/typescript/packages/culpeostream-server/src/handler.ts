/**
 * ICulpeoStreamHandler — application-level callbacks for server-side sessions.
 * IServerSession — per-connection session interface exposed to application handlers.
 *
 * Security invariants:
 *  - Session IDs must never be logged by handler implementations.
 *  - Auth tokens must never appear in errors or logs.
 */

import type { JsonObject, StreamDeclaration } from "culpeostream";

// ---------------------------------------------------------------------------
// IServerSession
// ---------------------------------------------------------------------------

/**
 * Represents an active server-side session.
 * Obtained from ICulpeoStreamHandler.onConnected and passed to all other callbacks.
 */
export interface IServerSession {
  /**
   * The opaque session identifier.
   *
   * SECURITY: Treat this as a secret. Do not log it.
   */
  readonly sessionId: string;

  /**
   * The confirmed stream declarations for this session, keyed by stream ID.
   * Available immediately after onConnected is called.
   */
  readonly streams: ReadonlyMap<string, StreamDeclaration>;

  /**
   * Send a media frame on the given stream.
   * The stream must exist and be output or duplex from the server perspective.
   */
  sendMedia(streamId: string, data: Uint8Array): Promise<void>;

  /**
   * Send an application-level event to the client.
   * The body must be a JSON-serializable object.
   */
  sendEvent(eventName: string, body: JsonObject): Promise<void>;

  /**
   * Send a culpeo.auth-refresh challenge to the client.
   * The nonce is managed internally by the core session state machine.
   */
  requestAuthRefresh(): Promise<void>;

  /**
   * Gracefully close the session with an optional close reason.
   * Sends culpeo.close and closes the underlying WebSocket.
   */
  close(reason?: string): Promise<void>;
}

// ---------------------------------------------------------------------------
// ICulpeoStreamHandler
// ---------------------------------------------------------------------------

/**
 * Application-level handler interface.
 * Implement this interface and pass it to createCulpeoServer.
 *
 * All methods are async — errors thrown from them are caught and do not
 * crash the server, but may result in the session being closed.
 */
export interface ICulpeoStreamHandler {
  /**
   * Called when a new session has been fully established (init-ack sent).
   * Use this to set up per-session state.
   */
  onConnected(session: IServerSession): Promise<void>;

  /**
   * Called when a media frame is received from the client.
   *
   * @param session  The active session.
   * @param streamId The stream on which the frame arrived.
   * @param data     The raw media payload.
   * @param offset   The frame offset (bigint to support large streams).
   */
  onMedia(
    session: IServerSession,
    streamId: string,
    data: Uint8Array,
    offset: bigint,
  ): Promise<void>;

  /**
   * Called when an application-level event frame is received from the client.
   *
   * @param session    The active session.
   * @param eventName  The event name (does not start with "culpeo.").
   * @param body       The JSON body of the event.
   */
  onEvent(
    session: IServerSession,
    eventName: string,
    body: JsonObject,
  ): Promise<void>;

  /**
   * Called when the session is disconnected, for any reason.
   * The session is no longer usable when this is called.
   *
   * @param session The session that disconnected.
   * @param reason  A human-readable reason string.
   */
  onDisconnected(session: IServerSession, reason: string): Promise<void>;

  /**
   * Optional error hook called when onMedia or onEvent throws.
   * If not provided, a console.warn is emitted instead.
   * The session remains alive after the error.
   *
   * @param session The active session at the time of the error.
   * @param error   The thrown value (any type — do not log auth tokens).
   */
  onError?: (session: IServerSession, error: unknown) => Promise<void>;
}
