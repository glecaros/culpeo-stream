/**
 * server.ts — CulpeoHttp2Server
 *
 * Accepts HTTP/2 POST requests and dispatches each stream to the application
 * handler as a CulpeoHttp2Connection.
 *
 * Security invariants:
 *  - Authorization header values MUST NOT be logged or included in errors.
 *  - allowInsecure MUST only be used for local development; a console.warn is
 *    emitted when it is set to true (mirrors the WebSocket client's wss guard).
 *  - maxPayloadBytes is enforced via decodeFrame to prevent memory exhaustion.
 */

import * as http2 from "node:http2";
import type {
  Http2Server,
  Http2SecureServer,
  ServerHttp2Stream,
  IncomingHttpHeaders,
} from "node:http2";

import type { CulpeoHttp2Connection } from "./connection.js";
import {
  CONTROL_FRAME,
  MEDIA_FRAME,
  decodeFrame,
  encodeFrame,
} from "./framing.js";

// ---------------------------------------------------------------------------
// Options & types
// ---------------------------------------------------------------------------

export interface CulpeoHttp2ServerOptions {
  /** TCP port to listen on. */
  port: number;
  /** PEM-encoded certificate (required unless allowInsecure is true). */
  cert?: string;
  /** PEM-encoded private key (required unless allowInsecure is true). */
  key?: string;
  /**
   * Use cleartext HTTP/2 (h2c) instead of TLS.
   * SECURITY: Must only be used for local development/testing.
   * A console.warn is emitted when this is true.
   * Default: false.
   */
  allowInsecure?: boolean;
  /**
   * Maximum frame payload size in bytes.
   * Frames exceeding this limit cause the stream to be reset.
   * Default: 16 MiB.
   */
  maxPayloadBytes?: number;
  /**
   * Path prefix to accept requests on.
   * Default: "/"
   */
  path?: string;
}

export type CulpeoHttp2Handler = (
  connection: CulpeoHttp2Connection,
) => Promise<void>;

// ---------------------------------------------------------------------------
// Internal connection implementation (server side)
// ---------------------------------------------------------------------------

class Http2ServerConnectionImpl implements CulpeoHttp2Connection {
  readonly authorizationHeader: string | undefined;

  private readonly stream: ServerHttp2Stream;
  private readonly maxPayloadBytes: number;
  private closed = false;

  constructor(
    stream: ServerHttp2Stream,
    authorizationHeader: string | undefined,
    maxPayloadBytes: number,
  ) {
    this.stream = stream;
    this.authorizationHeader = authorizationHeader;
    this.maxPayloadBytes = maxPayloadBytes;
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
    const maxPayloadBytes = this.maxPayloadBytes;

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
        let error: unknown = null;

        function flush() {
          while (buf.length >= 5) {
            const result = decodeFrame(buf, maxPayloadBytes);
            if (result === null) break;
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
          try {
            flush();
          } catch (err: unknown) {
            error = err;
            done = true;
            if (reject !== null) {
              const rej = reject;
              resolve = null;
              reject = null;
              rej(err);
            }
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
    try {
      this.stream.end();
    } catch {
      // ignore errors on already-closed streams
    }
  }
}

// ---------------------------------------------------------------------------
// CulpeoHttp2Server
// ---------------------------------------------------------------------------

/**
 * An HTTP/2 server that accepts CulpeoStream sessions.
 *
 * Usage:
 * ```ts
 * const server = new CulpeoHttp2Server(
 *   { port: 8443, cert, key },
 *   async (conn) => {
 *     for await (const frame of conn.frames()) { ... }
 *   },
 * );
 * await server.listen();
 * await server.close();
 * ```
 */
export class CulpeoHttp2Server {
  private readonly options: Required<CulpeoHttp2ServerOptions>;
  private readonly handler: CulpeoHttp2Handler;
  private server: Http2Server | Http2SecureServer | null = null;

  constructor(options: CulpeoHttp2ServerOptions, handler: CulpeoHttp2Handler) {
    if (
      !options.allowInsecure &&
      (options.cert === undefined || options.key === undefined)
    ) {
      throw new Error(
        "CulpeoHttp2Server: cert and key are required unless allowInsecure is true",
      );
    }
    if (options.allowInsecure === true) {
      console.warn(
        "[culpeostream-http2] CulpeoHttp2Server: allowInsecure is enabled. " +
          "This uses plaintext HTTP/2 (h2c) and must NOT be used in production.",
      );
    }
    this.options = {
      port: options.port,
      cert: options.cert ?? "",
      key: options.key ?? "",
      allowInsecure: options.allowInsecure ?? false,
      maxPayloadBytes: options.maxPayloadBytes ?? 16 * 1024 * 1024,
      path: options.path ?? "/",
    };
    this.handler = handler;
  }

  /** Start listening. Resolves when the server is bound to the port. */
  listen(): Promise<void> {
    return new Promise((resolve, reject) => {
      const { port, cert, key, allowInsecure, maxPayloadBytes, path } =
        this.options;

      const onStream = (
        stream: ServerHttp2Stream,
        headers: IncomingHttpHeaders,
      ) => {
        const method = headers[":method"];
        const reqPath = headers[":path"];

        // Only accept POST requests to the configured path.
        if (method !== "POST") {
          stream.respond({ ":status": 405 });
          stream.end();
          return;
        }

        if (typeof reqPath === "string" && reqPath !== path && path !== "/") {
          stream.respond({ ":status": 404 });
          stream.end();
          return;
        }

        // Respond 200 with CulpeoStream content-type.
        stream.respond({
          ":status": 200,
          "content-type": "application/culpeostream",
        });

        // SECURITY: authorizationHeader is extracted but MUST NOT be logged.
        const authorizationHeader =
          typeof headers["authorization"] === "string"
            ? headers["authorization"]
            : undefined;

        const conn = new Http2ServerConnectionImpl(
          stream,
          authorizationHeader,
          maxPayloadBytes,
        );

        this.handler(conn)
          .catch((err: unknown) => {
            console.error("[culpeostream-http2] Handler error:", err);
            conn.close();
          })
          .finally(() => {
            conn.close();
          });
      };

      let server: Http2Server | Http2SecureServer;

      if (allowInsecure) {
        server = http2.createServer();
      } else {
        server = http2.createSecureServer({ cert, key });
      }

      server.on("stream", onStream);

      server.on("error", (err: unknown) => {
        reject(err);
      });

      server.listen(port, () => {
        resolve();
      });

      this.server = server;
    });
  }

  /** Gracefully shut down the server. */
  close(): Promise<void> {
    return new Promise((resolve, reject) => {
      if (this.server === null) {
        resolve();
        return;
      }
      this.server.close((err) => {
        if (err != null) reject(err);
        else resolve();
      });
      this.server = null;
    });
  }
}
