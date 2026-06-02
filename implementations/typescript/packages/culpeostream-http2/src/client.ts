/**
 * client.ts — CulpeoHttp2Client
 *
 * Opens a single long-lived HTTP/2 POST stream to a CulpeoStream server and
 * returns a CulpeoHttp2Connection.
 *
 * Security invariants:
 *  - The authorization header value MUST NOT appear in Error messages or logs.
 *  - Self-signed certificates are only permitted via explicit opt-in
 *    (rejectUnauthorized: false) and are intended for test/dev use only.
 *  - TLS is required by default (wss-equivalent for HTTP/2); cleartext is
 *    permitted only via allowInsecure on the server side.
 */

import * as http2 from "node:http2";
import type { ClientHttp2Stream } from "node:http2";

import type { CulpeoHttp2Connection } from "./connection.js";
import {
  CONTROL_FRAME,
  MEDIA_FRAME,
  decodeFrame,
  encodeFrame,
} from "./framing.js";

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

export interface CulpeoHttp2ClientOptions {
  /** e.g. "https://localhost:8443" or "http://localhost:8080" (for h2c dev only) */
  authority: string;
  /** Path to POST to. Default: "/" */
  path?: string;
  /**
   * Value for the HTTP Authorization header.
   * SECURITY: Never log this value.
   */
  authorization?: string;
  /**
   * Set to false to accept self-signed TLS certificates.
   * SECURITY: Must not be set to false in production.
   * Default: true
   */
  rejectUnauthorized?: boolean;
}

// ---------------------------------------------------------------------------
// Internal connection implementation
// ---------------------------------------------------------------------------

class Http2ConnectionImpl implements CulpeoHttp2Connection {
  readonly authorizationHeader: string | undefined;

  private readonly stream: ClientHttp2Stream;
  private closed = false;

  constructor(stream: ClientHttp2Stream, authorization: string | undefined) {
    this.stream = stream;
    this.authorizationHeader = authorization;
  }

  sendControlFrame(payload: Uint8Array): Promise<void> {
    return this._write(encodeFrame(CONTROL_FRAME, payload));
  }

  sendMediaFrame(payload: Uint8Array): Promise<void> {
    return this._write(encodeFrame(MEDIA_FRAME, payload));
  }

  private _write(data: Buffer): Promise<void> {
    if (this.closed) {
      return Promise.reject(new Error("Connection is closed"));
    }
    return new Promise((resolve, reject) => {
      this.stream.write(data, (err) => {
        if (err != null) reject(err);
        else resolve();
      });
    });
  }

  frames(): AsyncIterable<{ typeOctet: number; payload: Buffer }> {
    const stream = this.stream;
    return {
      [Symbol.asyncIterator]() {
        let buf = Buffer.alloc(0);
        let resolve:
          | ((
              value: IteratorResult<{ typeOctet: number; payload: Buffer }>,
            ) => void)
          | null = null;
        let reject: ((err: unknown) => void) | null = null;
        const queue: { typeOctet: number; payload: Buffer }[] = [];
        let done = false;
        // SEC-026: store the first error so next() can surface it even after the
        // event handlers have already fired.
        let error: unknown = null;

        // SEC-032: validate type octet; unknown types are a protocol error.
        function flush() {
          while (buf.length >= 5) {
            const result = decodeFrame(buf);
            if (result === null) break;
            if (
              result.typeOctet !== CONTROL_FRAME &&
              result.typeOctet !== MEDIA_FRAME
            ) {
              throw new Error(
                `Unknown frame type octet 0x${result.typeOctet.toString(16).padStart(2, "0")}`,
              );
            }
            buf = buf.subarray(result.bytesConsumed);
            queue.push({
              typeOctet: result.typeOctet,
              payload: Buffer.from(result.payload),
            });
          }
        }

        stream.on("data", (chunk: Buffer | string) => {
          const data = Buffer.isBuffer(chunk)
            ? chunk
            : Buffer.from(chunk as string);
          buf = Buffer.concat([buf, data]);
          // SEC-026: catch RangeError (oversized frame) and SEC-032: unknown type
          // octet.  Destroy the stream so the 'error' event fires and the
          // async iterator surfaces a clean rejection to the consumer.
          try {
            flush();
          } catch (err) {
            stream.destroy(err instanceof Error ? err : new Error(String(err)));
            return;
          }
          if (resolve !== null && queue.length > 0) {
            const item = queue.shift()!;
            const res = resolve;
            resolve = null;
            reject = null;
            res({ value: item, done: false });
          }
        });

        stream.on("end", () => {
          done = true;
          if (resolve !== null) {
            const res = resolve;
            resolve = null;
            reject = null;
            res({
              value: undefined as unknown as {
                typeOctet: number;
                payload: Buffer;
              },
              done: true,
            });
          }
        });

        stream.on("error", (err: unknown) => {
          done = true;
          // SEC-026: store error so next() can reject even if called after the
          // event has already fired (e.g. when the consumer is processing a
          // queued frame and the error arrives between two next() calls).
          error = err;
          if (reject !== null) {
            const rej = reject;
            resolve = null;
            reject = null;
            rej(err);
          }
        });

        return {
          next(): Promise<
            IteratorResult<{ typeOctet: number; payload: Buffer }>
          > {
            if (queue.length > 0) {
              return Promise.resolve({ value: queue.shift()!, done: false });
            }
            if (done) {
              // SEC-026: surface the stored error rather than returning
              // {done:true}, so the consumer sees a clean rejection.
              if (error !== null) return Promise.reject(error);
              return Promise.resolve({
                value: undefined as unknown as {
                  typeOctet: number;
                  payload: Buffer;
                },
                done: true,
              });
            }
            return new Promise((res, rej) => {
              resolve = res;
              reject = rej;
            });
          },
          return(): Promise<
            IteratorResult<{ typeOctet: number; payload: Buffer }>
          > {
            done = true;
            return Promise.resolve({
              value: undefined as unknown as {
                typeOctet: number;
                payload: Buffer;
              },
              done: true,
            });
          },
        };
      },
    };
  }

  close(): void {
    if (this.closed) return;
    this.closed = true;
    this.stream.end();
  }
}

// ---------------------------------------------------------------------------
// CulpeoHttp2Client
// ---------------------------------------------------------------------------

/**
 * Opens a persistent HTTP/2 POST stream to a CulpeoStream server.
 *
 * Usage:
 * ```ts
 * const client = new CulpeoHttp2Client({ authority: 'https://localhost:8443' });
 * const conn = await client.connect();
 * await conn.sendControlFrame(payload);
 * for await (const frame of conn.frames()) { ... }
 * client.close();
 * ```
 */
export class CulpeoHttp2Client {
  private readonly options: Required<
    Omit<CulpeoHttp2ClientOptions, "authorization">
  > & { authorization?: string };
  private session: http2.ClientHttp2Session | null = null;

  constructor(options: CulpeoHttp2ClientOptions) {
    // SEC-027: Warn once at construction time when TLS verification is disabled.
    // SECURITY: Must only be used in local development/testing — never in production.
    if (options.rejectUnauthorized === false) {
      console.warn(
        "[culpeostream-http2] WARNING: rejectUnauthorized is false. " +
          "TLS certificate verification is disabled. " +
          "This must only be used in local development/testing.",
      );
    }
    this.options = {
      authority: options.authority,
      path: options.path ?? "/",
      rejectUnauthorized: options.rejectUnauthorized ?? true,
      ...(options.authorization !== undefined
        ? { authorization: options.authorization }
        : {}),
    };
  }

  /**
   * Connect to the server and return a ready-to-use CulpeoHttp2Connection.
   *
   * Resolves when the server responds with HTTP 200.
   * Rejects if the server returns a non-200 status.
   */
  connect(): Promise<CulpeoHttp2Connection> {
    return new Promise((resolve, reject) => {
      const { authority, path, rejectUnauthorized, authorization } =
        this.options;

      const session = http2.connect(authority, { rejectUnauthorized });
      this.session = session;

      session.on("error", (err: unknown) => {
        reject(err);
      });

      const requestHeaders: http2.OutgoingHttpHeaders = {
        ":method": "POST",
        ":path": path,
        "content-type": "application/culpeostream",
        "culpeostream-version": "1.0",
      };

      // SECURITY: authorization header is set only when provided; never logged.
      if (authorization !== undefined) {
        requestHeaders["authorization"] = authorization;
      }

      const stream = session.request(requestHeaders);

      stream.on("response", (headers) => {
        const status = headers[":status"];
        if (status !== 200) {
          stream.destroy();
          reject(
            new Error(
              `CulpeoStream HTTP/2: server responded with status ${String(status)}`,
            ),
          );
          return;
        }
        resolve(new Http2ConnectionImpl(stream, authorization));
      });

      stream.on("error", (err: unknown) => {
        reject(err);
      });
    });
  }

  /**
   * Close the underlying HTTP/2 session.
   * Idempotent.
   */
  close(): void {
    if (this.session !== null) {
      this.session.close();
      this.session = null;
    }
  }
}
