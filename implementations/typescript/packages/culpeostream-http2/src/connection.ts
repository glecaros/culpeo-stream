/**
 * connection.ts — CulpeoHttp2Connection interface.
 *
 * A CulpeoHttp2Connection wraps a single HTTP/2 stream (one per CulpeoStream
 * session) and provides a frame-oriented API over the raw byte stream.
 *
 * Security invariants:
 *  - The authorizationHeader field, if populated, MUST NOT be logged or
 *    included in Error objects.
 *  - close() is idempotent; callers need not guard against double-close.
 */

/**
 * Represents one active CulpeoStream session carried over a single HTTP/2
 * stream.  Both the client and server sides implement this interface.
 */
export interface CulpeoHttp2Connection {
  /**
   * Send a control/event frame (type octet 0x01).
   *
   * @param payload  Raw serialized CulpeoStream frame bytes (header block + JSON body).
   */
  sendControlFrame(payload: Uint8Array): Promise<void>;

  /**
   * Send a media frame (type octet 0x02).
   *
   * @param payload  Raw serialized CulpeoStream frame bytes (header block + binary body).
   */
  sendMediaFrame(payload: Uint8Array): Promise<void>;

  /**
   * Async iterable of incoming frames decoded from the HTTP/2 DATA stream.
   *
   * Yields `{ typeOctet, payload }` for each complete frame received.
   * The iterator ends when the remote side closes the stream.
   *
   * IMPORTANT: Only one consumer may iterate at a time.
   */
  frames(): AsyncIterable<{ typeOctet: number; payload: Buffer }>;

  /**
   * The value of the HTTP `Authorization` header sent by the client, or
   * `undefined` if not present.
   *
   * SECURITY: This field contains credentials. Never log it.
   */
  readonly authorizationHeader: string | undefined;

  /**
   * Half-close the outgoing direction and release resources.
   * Idempotent — safe to call multiple times.
   */
  close(): void;
}
