/**
 * client-server.test.ts — Integration tests for CulpeoHttp2Client ↔ CulpeoHttp2Server.
 *
 * All tests use allowInsecure: true + rejectUnauthorized: false (no TLS cert in CI).
 *
 * Test list:
 *  6.  client sends a control frame, server handler receives it
 *  7.  client sends a media frame, server handler receives it
 *  8.  server handler sends a control frame, client receives it
 *  9.  server handler sends a media frame, client receives it
 *  10. bidirectional: client and server exchange 10 frames each, all arrive in order
 *  11. large frame (64 KB payload) round-trips correctly
 *  12. connection close: server closes, client frames() iterator ends
 *  13. authorization header is forwarded to server
 *  14. interop: culpeostream core serializes culpeo.init, sent over HTTP/2, received and parseable
 */

import { afterEach, describe, expect, it } from "vitest";

import { CONTROL_FRAME, MEDIA_FRAME } from "../src/framing.js";
import { CulpeoHttp2Client } from "../src/client.js";
import { CulpeoHttp2Server } from "../src/server.js";
import type { CulpeoHttp2Connection } from "../src/connection.js";

// Import core serializer/parser for the interop test
import { parseFrame, serializeFrame } from "culpeostream";
import type { InitFrame } from "culpeostream";

// ---------------------------------------------------------------------------
// Port allocator — each test gets a unique port
// ---------------------------------------------------------------------------
let portSeed = 19100;
function allocPort(): number {
  return portSeed++;
}

// ---------------------------------------------------------------------------
// Test rig helpers
// ---------------------------------------------------------------------------

interface Rig {
  server: CulpeoHttp2Server;
  client: CulpeoHttp2Client;
  port: number;
  teardown(): Promise<void>;
}

/**
 * Create a server with the given handler and a matching client.
 * Returns a rig that cleans up both on teardown().
 */
async function makeRig(
  handler: (conn: CulpeoHttp2Connection) => Promise<void>,
): Promise<Rig> {
  const port = allocPort();
  const server = new CulpeoHttp2Server({ port, allowInsecure: true }, handler);
  await server.listen();
  const client = new CulpeoHttp2Client({
    authority: `http://localhost:${port}`,
    rejectUnauthorized: false,
  });
  return {
    port,
    server,
    client,
    async teardown() {
      client.close();
      await server.close();
    },
  };
}

/** Collect up to `limit` frames from a connection. */
async function collectFrames(
  conn: CulpeoHttp2Connection,
  limit: number,
): Promise<Array<{ typeOctet: number; payload: Buffer }>> {
  const out: Array<{ typeOctet: number; payload: Buffer }> = [];
  for await (const frame of conn.frames()) {
    out.push(frame);
    if (out.length >= limit) break;
  }
  return out;
}

/** Create a deferred promise (resolve/reject exposed). */
function deferred<T = void>(): {
  promise: Promise<T>;
  resolve: (value: T) => void;
  reject: (reason?: unknown) => void;
} {
  let resolve!: (value: T) => void;
  let reject!: (reason?: unknown) => void;
  const promise = new Promise<T>((res, rej) => {
    resolve = res;
    reject = rej;
  });
  return { promise, resolve, reject };
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

describe("CulpeoHttp2Client ↔ CulpeoHttp2Server (h2c, insecure)", () => {
  let rig: Rig | null = null;

  afterEach(async () => {
    if (rig !== null) {
      await rig.teardown();
      rig = null;
    }
  });

  // -----------------------------------------------------------------------
  // 6. Client sends a control frame, server handler receives it
  // -----------------------------------------------------------------------
  it("client can send a control frame, server handler receives it", async () => {
    const gate = deferred<{ typeOctet: number; payload: Buffer }>();

    rig = await makeRig(async (conn) => {
      for await (const frame of conn.frames()) {
        gate.resolve(frame);
        break;
      }
    });

    const conn = await rig.client.connect();
    const payload = Buffer.from(
      "Event: culpeo.ping\r\nContent-Type: application/json\r\n\r\n{}",
    );
    await conn.sendControlFrame(payload);

    const received = await gate.promise;
    expect(received.typeOctet).toBe(CONTROL_FRAME);
    expect(Buffer.compare(received.payload, payload)).toBe(0);
    conn.close();
  });

  // -----------------------------------------------------------------------
  // 7. Client sends a media frame, server handler receives it
  // -----------------------------------------------------------------------
  it("client can send a media frame, server handler receives it", async () => {
    const gate = deferred<{ typeOctet: number; payload: Buffer }>();

    rig = await makeRig(async (conn) => {
      for await (const frame of conn.frames()) {
        gate.resolve(frame);
        break;
      }
    });

    const conn = await rig.client.connect();
    const payload = Buffer.alloc(32, 0xcc);
    await conn.sendMediaFrame(payload);

    const received = await gate.promise;
    expect(received.typeOctet).toBe(MEDIA_FRAME);
    expect(Buffer.compare(received.payload, payload)).toBe(0);
    conn.close();
  });

  // -----------------------------------------------------------------------
  // 8. Server handler sends a control frame, client receives it
  // -----------------------------------------------------------------------
  it("server handler can send a control frame, client receives it", async () => {
    const serverPayload = Buffer.from(
      "Event: culpeo.init-ack\r\nSession-Id: s1\r\nContent-Type: application/json\r\n\r\n{}",
    );

    rig = await makeRig(async (conn) => {
      await conn.sendControlFrame(serverPayload);
      conn.close();
    });

    const conn = await rig.client.connect();
    const frames = await collectFrames(conn, 1);
    expect(frames.length).toBe(1);
    expect(frames[0]!.typeOctet).toBe(CONTROL_FRAME);
    expect(Buffer.compare(frames[0]!.payload, serverPayload)).toBe(0);
    conn.close();
  });

  // -----------------------------------------------------------------------
  // 9. Server handler sends a media frame, client receives it
  // -----------------------------------------------------------------------
  it("server handler can send a media frame, client receives it", async () => {
    const serverPayload = Buffer.alloc(64, 0xde);

    rig = await makeRig(async (conn) => {
      await conn.sendMediaFrame(serverPayload);
      conn.close();
    });

    const conn = await rig.client.connect();
    const frames = await collectFrames(conn, 1);
    expect(frames.length).toBe(1);
    expect(frames[0]!.typeOctet).toBe(MEDIA_FRAME);
    expect(Buffer.compare(frames[0]!.payload, serverPayload)).toBe(0);
    conn.close();
  });

  // -----------------------------------------------------------------------
  // 10. Bidirectional: 10 frames each, all arrive in order
  // -----------------------------------------------------------------------
  it("bidirectional: client and server exchange 10 frames each, all arrive in order", async () => {
    const serverReceived: string[] = [];
    const serverDone = deferred();

    rig = await makeRig(async (conn) => {
      // Send 10 frames to the client
      for (let i = 0; i < 10; i++) {
        await conn.sendControlFrame(Buffer.from(`server-frame-${i}`));
      }
      // Receive 10 frames from the client
      let count = 0;
      for await (const frame of conn.frames()) {
        serverReceived.push(frame.payload.toString());
        count++;
        if (count >= 10) break;
      }
      serverDone.resolve();
    });

    const conn = await rig.client.connect();

    // Collect 10 frames from server concurrently with sending
    const clientReceivedPromise = collectFrames(conn, 10);

    // Send 10 frames to server
    for (let i = 0; i < 10; i++) {
      await conn.sendControlFrame(Buffer.from(`client-frame-${i}`));
    }

    const clientReceived = await clientReceivedPromise;
    await serverDone.promise;
    conn.close();

    expect(clientReceived.length).toBe(10);
    for (let i = 0; i < 10; i++) {
      expect(clientReceived[i]!.payload.toString()).toBe(`server-frame-${i}`);
    }

    expect(serverReceived.length).toBe(10);
    for (let i = 0; i < 10; i++) {
      expect(serverReceived[i]).toBe(`client-frame-${i}`);
    }
  });

  // -----------------------------------------------------------------------
  // 11. Large frame (64 KB payload) round-trips correctly
  // -----------------------------------------------------------------------
  it("large frame (64 KB payload) round-trips correctly", async () => {
    const SIZE = 64 * 1024;
    const largePayload = Buffer.allocUnsafe(SIZE);
    for (let i = 0; i < SIZE; i++) {
      largePayload[i] = i & 0xff;
    }

    rig = await makeRig(async (conn) => {
      await conn.sendMediaFrame(largePayload);
      conn.close();
    });

    const conn = await rig.client.connect();
    const frames = await collectFrames(conn, 1);

    expect(frames.length).toBe(1);
    expect(frames[0]!.typeOctet).toBe(MEDIA_FRAME);
    expect(frames[0]!.payload.length).toBe(SIZE);
    expect(Buffer.compare(frames[0]!.payload, largePayload)).toBe(0);
    conn.close();
  });

  // -----------------------------------------------------------------------
  // 12. Connection close: server closes, client frames() iterator ends
  // -----------------------------------------------------------------------
  it("connection close: server closes, client frames() iterator ends", async () => {
    const closePayload = Buffer.from(
      "Event: culpeo.close\r\nCode: normal\r\nReason: done\r\n\r\n{}",
    );

    rig = await makeRig(async (conn) => {
      await conn.sendControlFrame(closePayload);
      conn.close();
    });

    const conn = await rig.client.connect();
    const frames: Array<{ typeOctet: number; payload: Buffer }> = [];

    // Should end naturally after server closes
    for await (const frame of conn.frames()) {
      frames.push(frame);
    }

    expect(frames.length).toBeGreaterThanOrEqual(1);
    expect(frames[0]!.typeOctet).toBe(CONTROL_FRAME);
    conn.close();
  });

  // -----------------------------------------------------------------------
  // 13. Authorization header is forwarded to server
  // -----------------------------------------------------------------------
  it("authorization header is forwarded to server", async () => {
    const authGate = deferred<string | undefined>();

    const port = allocPort();
    const server = new CulpeoHttp2Server(
      { port, allowInsecure: true },
      async (conn) => {
        // SECURITY: we capture the value only for assertion; must not log it
        authGate.resolve(conn.authorizationHeader);
        conn.close();
      },
    );
    await server.listen();
    const client = new CulpeoHttp2Client({
      authority: `http://localhost:${port}`,
      rejectUnauthorized: false,
      authorization: "Bearer test-token-abc123",
    });
    rig = {
      port,
      server,
      client,
      teardown: async () => {
        client.close();
        await server.close();
      },
    };

    const conn = await client.connect();
    conn.close();

    const receivedHeader = await authGate.promise;
    expect(receivedHeader).toBe("Bearer test-token-abc123");
  });

  // -----------------------------------------------------------------------
  // 14. Interop: culpeostream core serializes culpeo.init, sent over HTTP/2,
  //     received and parseable on the other side
  // -----------------------------------------------------------------------
  it("interop: culpeostream core serializes culpeo.init, sent over HTTP/2, received and parseable", async () => {
    const gate = deferred<{ typeOctet: number; payload: Buffer }>();

    rig = await makeRig(async (conn) => {
      for await (const frame of conn.frames()) {
        gate.resolve(frame);
        break;
      }
    });

    // Serialize a real culpeo.init frame with the core library
    const initFrame: InitFrame = {
      kind: "control",
      event: "culpeo.init",
      headers: {
        event: "culpeo.init",
        authorization: "Bearer interop-test",
        contentType: "application/json",
      },
      body: {
        version: "1.0",
        streams: [
          {
            id: "audio",
            type: "input",
            content_type: "audio/opus",
            offset_type: "message",
            purpose: "voice",
          },
        ],
      },
    };

    const serialized = serializeFrame(initFrame);
    expect(serialized.frameType).toBe("text");
    const payloadBytes =
      serialized.frameType === "text"
        ? new TextEncoder().encode(serialized.data)
        : serialized.data;

    const conn = await rig.client.connect();
    await conn.sendControlFrame(payloadBytes);
    conn.close();

    // Verify the server received a parseable culpeo.init frame
    const received = await gate.promise;
    expect(received.typeOctet).toBe(CONTROL_FRAME);

    const parsed = parseFrame(received.payload, "text");
    expect(parsed.kind).toBe("control");
    if (parsed.kind === "control") {
      expect(parsed.event).toBe("culpeo.init");
    }
  });
});
