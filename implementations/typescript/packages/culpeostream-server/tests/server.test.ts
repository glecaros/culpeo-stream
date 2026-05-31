/**
 * Tests for culpeostream-server.
 *
 * Uses ws on both sides:
 *  - Server: CulpeoServer (via createCulpeoServer) attached to an http.Server.
 *  - Client: raw ws.WebSocket + culpeostream frame utilities for protocol-level control.
 *
 * Security note: authorization values in tests are non-sensitive constants.
 */

import * as http from "node:http";
import {
  afterEach,
  beforeEach,
  describe,
  expect,
  it,
  vi,
  type MockedFunction,
} from "vitest";
import WebSocket from "ws";

import { parseFrame, serializeFrame } from "culpeostream";
import type {
  ConfirmedStreamDeclaration,
  InitAckFrame,
  InitErrorFrame,
  ResumeStreamDeclaration,
} from "culpeostream";

import { createCulpeoServer, InMemorySessionStore } from "../src/index.js";
import type {
  CulpeoServerOptions,
  ICulpeoStreamHandler,
  IServerSession,
} from "../src/index.js";

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

const VALID_TOKEN = "Bearer valid-token";
const INVALID_TOKEN = "Bearer bad-token";

const DEFAULT_STREAM: ResumeStreamDeclaration = {
  type: "input",
  content_type: "audio/opus",
  offset_type: "message",
  purpose: "voice",
};

/** Flush all pending microtasks (several layers deep). */
async function tick(n = 20): Promise<void> {
  for (let i = 0; i < n; i++) {
    await Promise.resolve();
  }
}

/** Sleep for real milliseconds (needed for timer tests). */
function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

/**
 * Serialise a culpeo.init frame as a text string ready to send over ws.
 */
function makeInitText(opts: {
  streams?: ResumeStreamDeclaration[];
  token?: string;
  sessionId?: string;
  bufferWindowMs?: number;
}): string {
  const {
    streams = [DEFAULT_STREAM],
    token = VALID_TOKEN,
    sessionId,
    bufferWindowMs = 5000,
  } = opts;

  const serialized = serializeFrame({
    kind: "control",
    event: "culpeo.init",
    headers: {
      event: "culpeo.init",
      authorization: token,
      contentType: "application/json",
      bufferWindow: bufferWindowMs,
      ...(sessionId !== undefined ? { sessionId } : {}),
    },
    body: { version: "0.3", streams },
  });

  if (serialized.frameType !== "text") throw new Error("Expected text frame");
  return serialized.data;
}

/**
 * Parse a received ws message into a CulpeoMessage.
 */
function parseWsMessage(
  data: WebSocket.RawData,
  isBinary: boolean,
): ReturnType<typeof parseFrame> {
  if (isBinary) {
    const buf = Buffer.isBuffer(data)
      ? data
      : data instanceof ArrayBuffer
        ? Buffer.from(data)
        : Buffer.concat(data as Buffer[]);
    return parseFrame(
      new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength),
      "binary",
    );
  }
  return parseFrame(
    Buffer.isBuffer(data) ? data.toString("utf-8") : String(data),
    "text",
  );
}

/**
 * Connect a ws client, send culpeo.init, and wait for init-ack.
 * Resolves with the ws client and the init-ack frame.
 * Rejects if init-error is received or connection fails.
 */
function connectAndHandshake(
  port: number,
  opts: {
    streams?: ResumeStreamDeclaration[];
    token?: string;
    sessionId?: string;
    bufferWindowMs?: number;
  } = {},
): Promise<{ ws: WebSocket; ack: InitAckFrame }> {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(`ws://127.0.0.1:${port}`);

    ws.once("open", () => {
      ws.send(makeInitText(opts));
    });

    const onMessage = (data: WebSocket.RawData, isBinary: boolean): void => {
      try {
        const frame = parseWsMessage(data, isBinary);
        if (frame.kind === "control" && frame.event === "culpeo.init-ack") {
          ws.removeListener("message", onMessage);
          resolve({ ws, ack: frame as InitAckFrame });
        } else if (
          frame.kind === "control" &&
          frame.event === "culpeo.init-error"
        ) {
          ws.removeListener("message", onMessage);
          const errorFrame = frame as InitErrorFrame;
          reject(
            new Error(
              `init-error: ${errorFrame.headers.code} — ${errorFrame.headers.reason}`,
            ),
          );
        }
      } catch (err) {
        reject(err);
      }
    };

    ws.on("message", onMessage);
    ws.once("error", reject);
  });
}

/** Get a free port by letting the OS assign one. */
function getFreePort(): Promise<number> {
  return new Promise((resolve, reject) => {
    const server = http.createServer();
    server.listen(0, "127.0.0.1", () => {
      const addr = server.address();
      if (addr === null || typeof addr === "string") {
        reject(new Error("Unexpected address type"));
        return;
      }
      const { port } = addr;
      server.close(() => resolve(port));
    });
    server.once("error", reject);
  });
}

/** Build a no-op handler (all methods are no-ops). */
function makeNoOpHandler(): ICulpeoStreamHandler {
  return {
    onConnected: async () => {},
    onMedia: async () => {},
    onEvent: async () => {},
    onDisconnected: async () => {},
  };
}

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

interface TestContext {
  port: number;
  server: ReturnType<typeof createCulpeoServer>;
  httpServer: http.Server;
  store: InMemorySessionStore;
  authenticate: MockedFunction<CulpeoServerOptions["authenticate"]>;
  handler: {
    onConnected: MockedFunction<ICulpeoStreamHandler["onConnected"]>;
    onMedia: MockedFunction<ICulpeoStreamHandler["onMedia"]>;
    onEvent: MockedFunction<ICulpeoStreamHandler["onEvent"]>;
    onDisconnected: MockedFunction<ICulpeoStreamHandler["onDisconnected"]>;
  };
}

async function createTestContext(
  optionOverrides: Partial<
    Omit<CulpeoServerOptions, "authenticate" | "handler">
  > = {},
): Promise<TestContext> {
  const port = await getFreePort();
  const store = new InMemorySessionStore();

  const authenticate = vi.fn(
    async (token: string, _sessionId?: string): Promise<boolean> =>
      token === VALID_TOKEN,
  );

  const handler = {
    onConnected: vi.fn<[IServerSession], Promise<void>>(async () => {}),
    onMedia: vi.fn<[IServerSession, string, Uint8Array, bigint], Promise<void>>(
      async () => {},
    ),
    onEvent: vi.fn<
      [IServerSession, string, Record<string, unknown>],
      Promise<void>
    >(async () => {}),
    onDisconnected: vi.fn<[IServerSession, string], Promise<void>>(
      async () => {},
    ),
  };

  const httpServer = http.createServer();
  const server = createCulpeoServer({
    authenticate,
    handler,
    sessionStore: store,
    pingIntervalMs: 0, // disable by default; individual tests opt in
    ...optionOverrides,
  });

  server.attach(httpServer);

  await new Promise<void>((resolve, reject) => {
    httpServer.once("error", reject);
    httpServer.listen(port, "127.0.0.1", () => {
      httpServer.removeListener("error", reject);
      resolve();
    });
  });

  return { port, server, httpServer, store, authenticate, handler };
}

async function teardownTestContext(ctx: TestContext): Promise<void> {
  await ctx.server.close();
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

describe("CulpeoServer — init handshake", () => {
  let ctx: TestContext;

  beforeEach(async () => {
    ctx = await createTestContext();
  });

  afterEach(async () => {
    await teardownTestContext(ctx);
  });

  it("completes a full init handshake and calls onConnected", async () => {
    let connectedSession: IServerSession | undefined;
    ctx.handler.onConnected.mockImplementation(async (s) => {
      connectedSession = s;
    });

    const { ws, ack } = await connectAndHandshake(ctx.port);

    expect(ack.event).toBe("culpeo.init-ack");
    expect(ack.headers.sessionId).toBeTruthy();
    expect(ack.body.version).toBe("0.3");
    expect(ack.body.streams).toHaveLength(1);
    expect(ack.body.streams[0]?.content_type).toBe("audio/opus");

    await tick();

    expect(ctx.handler.onConnected).toHaveBeenCalledOnce();
    expect(connectedSession).toBeDefined();
    // Session ID should be set on the server session
    expect(connectedSession?.sessionId).toBeTruthy();
    // Streams map should contain the confirmed stream
    expect(connectedSession?.streams.size).toBe(1);

    ws.close();
    await sleep(50);
  });

  it("passes the correct stream declarations to onConnected", async () => {
    const streams: ResumeStreamDeclaration[] = [
      {
        type: "input",
        content_type: "audio/opus",
        offset_type: "message",
        purpose: "voice",
      },
      {
        type: "output",
        content_type: "audio/pcm",
        offset_type: "byte",
        purpose: "synthesis",
      },
    ];

    const { ws, ack } = await connectAndHandshake(ctx.port, { streams });
    await tick();

    expect(ack.body.streams).toHaveLength(2);
    const session = ctx.handler.onConnected.mock.calls[0]?.[0];
    expect(session?.streams.size).toBe(2);

    ws.close();
    await sleep(50);
  });

  it("returns version in init-ack", async () => {
    const { ws, ack } = await connectAndHandshake(ctx.port);
    expect(ack.body.version).toBe("0.3");
    ws.close();
    await sleep(50);
  });
});

describe("CulpeoServer — authentication", () => {
  let ctx: TestContext;

  beforeEach(async () => {
    ctx = await createTestContext();
  });

  afterEach(async () => {
    await teardownTestContext(ctx);
  });

  it("rejects connections with a bad token (init-error unauthorized)", async () => {
    await expect(
      connectAndHandshake(ctx.port, { token: INVALID_TOKEN }),
    ).rejects.toThrow("init-error: unauthorized");
  });

  it("does not call onConnected for rejected sessions", async () => {
    await expect(
      connectAndHandshake(ctx.port, { token: INVALID_TOKEN }),
    ).rejects.toThrow();

    await tick();
    expect(ctx.handler.onConnected).not.toHaveBeenCalled();
  });

  it("rejects connections with an empty token", async () => {
    await expect(
      connectAndHandshake(ctx.port, { token: "" }),
    ).rejects.toThrow();
  });

  it("accepts valid tokens", async () => {
    const { ws, ack } = await connectAndHandshake(ctx.port, {
      token: VALID_TOKEN,
    });
    expect(ack.event).toBe("culpeo.init-ack");
    ws.close();
    await sleep(50);
  });

  it("calls authenticate with the Authorization header value and no sessionId for new sessions", async () => {
    const { ws } = await connectAndHandshake(ctx.port, {
      token: VALID_TOKEN,
    });
    expect(ctx.authenticate).toHaveBeenCalledWith(VALID_TOKEN, undefined);
    ws.close();
    await sleep(50);
  });
});

describe("CulpeoServer — session resumption", () => {
  let ctx: TestContext;

  beforeEach(async () => {
    ctx = await createTestContext({ pingIntervalMs: 0 });
  });

  afterEach(async () => {
    await teardownTestContext(ctx);
  });

  it("resumes a session using a stored snapshot", async () => {
    // First connection — establish and record the session ID.
    const { ws: ws1, ack: ack1 } = await connectAndHandshake(ctx.port);
    const sessionId = ack1.headers.sessionId;
    await tick();

    // Disconnect so the session is saved to the store.
    await new Promise<void>((resolve) => {
      ws1.once("close", () => resolve());
      ws1.close();
    });
    await sleep(50);

    // The store should have the session saved.
    const saved = await ctx.store.load(sessionId);
    expect(saved).not.toBeNull();
    expect(saved?.sessionId).toBe(sessionId);

    // Second connection — resume with the same session ID.
    const { ws: ws2, ack: ack2 } = await connectAndHandshake(ctx.port, {
      sessionId,
    });

    // Should get the same session ID back.
    expect(ack2.headers.sessionId).toBe(sessionId);
    expect(ack2.body.streams).toHaveLength(1);

    ws2.close();
    await sleep(50);
  });

  it("returns init-error for an unknown session ID", async () => {
    // Attempt to resume a non-existent session.
    await expect(
      connectAndHandshake(ctx.port, {
        sessionId: "nonexistent-session-id",
      }),
    ).rejects.toThrow("init-error: invalid-session");
  });
});

describe("CulpeoServer — media frame routing", () => {
  let ctx: TestContext;

  beforeEach(async () => {
    ctx = await createTestContext();
  });

  afterEach(async () => {
    await teardownTestContext(ctx);
  });

  it("routes incoming media frames to onMedia handler", async () => {
    const received: Array<{
      streamId: string;
      data: Uint8Array;
      offset: bigint;
    }> = [];
    ctx.handler.onMedia.mockImplementation(
      async (_, streamId, data, offset) => {
        received.push({ streamId, data, offset });
      },
    );

    const { ws, ack } = await connectAndHandshake(ctx.port);
    await tick();

    const streamId = ack.body.streams[0]?.id;
    expect(streamId).toBeTruthy();

    const payload = new Uint8Array([1, 2, 3, 4, 5]);
    const mediaFrame = serializeFrame({
      kind: "media",
      headers: {
        streamId: streamId!,
        offset: 0,
        contentType: "audio/opus",
      },
      body: payload,
    });

    ws.send(mediaFrame.data);
    await sleep(80);

    expect(received).toHaveLength(1);
    expect(received[0]?.streamId).toBe(streamId);
    expect(received[0]?.data).toEqual(payload);
    expect(received[0]?.offset).toBe(0n);

    ws.close();
    await sleep(50);
  });

  it("routes a second media frame with the correct sequential offset", async () => {
    const offsets: bigint[] = [];
    ctx.handler.onMedia.mockImplementation(
      async (_, _streamId, _data, offset) => {
        offsets.push(offset);
      },
    );

    const { ws, ack } = await connectAndHandshake(ctx.port);
    await tick();

    const streamId = ack.body.streams[0]?.id;
    expect(streamId).toBeTruthy();

    // Send two frames with offsets 0 and 1 (message offset type).
    for (let i = 0; i < 2; i++) {
      const frame = serializeFrame({
        kind: "media",
        headers: {
          streamId: streamId!,
          offset: i,
          contentType: "audio/opus",
        },
        body: new Uint8Array([i]),
      });
      ws.send(frame.data);
    }

    await sleep(80);

    expect(offsets).toHaveLength(2);
    expect(offsets[0]).toBe(0n);
    expect(offsets[1]).toBe(1n);

    ws.close();
    await sleep(50);
  });
});

describe("CulpeoServer — application event routing", () => {
  let ctx: TestContext;

  beforeEach(async () => {
    ctx = await createTestContext();
  });

  afterEach(async () => {
    await teardownTestContext(ctx);
  });

  it("routes application events to onEvent handler", async () => {
    const events: Array<{ name: string; body: unknown }> = [];
    ctx.handler.onEvent.mockImplementation(async (_, eventName, body) => {
      events.push({ name: eventName, body });
    });

    const { ws } = await connectAndHandshake(ctx.port);
    await tick();

    const eventFrame = serializeFrame({
      kind: "control",
      event: "my-app.hello",
      headers: { event: "my-app.hello", contentType: "application/json" },
      body: { greeting: "hello world" },
    });

    ws.send(eventFrame.data);
    await sleep(80);

    expect(events).toHaveLength(1);
    expect(events[0]?.name).toBe("my-app.hello");
    expect(events[0]?.body).toEqual({ greeting: "hello world" });

    ws.close();
    await sleep(50);
  });

  it("does not route culpeo.* protocol events to onEvent", async () => {
    const { ws } = await connectAndHandshake(ctx.port);
    await tick();

    // Send a ping — should not appear in onEvent
    const pingFrame = serializeFrame({
      kind: "control",
      event: "culpeo.ping",
      headers: { event: "culpeo.ping", contentType: "application/json" },
      body: { ts: Date.now() * 1000 },
    });

    ws.send(pingFrame.data);
    await sleep(80);

    expect(ctx.handler.onEvent).not.toHaveBeenCalled();

    ws.close();
    await sleep(50);
  });
});

describe("CulpeoServer — server-initiated send", () => {
  let ctx: TestContext;

  beforeEach(async () => {
    ctx = await createTestContext();
  });

  afterEach(async () => {
    await teardownTestContext(ctx);
  });

  it("sends a server-initiated event to the client", async () => {
    let connectedSession: IServerSession | undefined;
    ctx.handler.onConnected.mockImplementation(async (session) => {
      connectedSession = session;
    });

    const { ws, ack } = await connectAndHandshake(ctx.port);
    await tick();

    expect(connectedSession).toBeDefined();

    const received: ReturnType<typeof parseFrame>[] = [];
    ws.on("message", (data, isBinary) => {
      received.push(parseWsMessage(data, isBinary));
    });

    await connectedSession!.sendEvent("app.notify", { msg: "test" });
    await sleep(80);

    const appEvent = received.find(
      (f) => f.kind === "control" && f.event === "app.notify",
    );
    expect(appEvent).toBeDefined();

    ws.close();
    await sleep(50);
  });

  it("sends server-initiated media to the client on a duplex stream", async () => {
    let connectedSession: IServerSession | undefined;
    ctx.handler.onConnected.mockImplementation(async (session) => {
      connectedSession = session;
    });

    // Use a duplex stream so the server can send media on it.
    const streams: ResumeStreamDeclaration[] = [
      {
        type: "duplex",
        content_type: "audio/opus",
        offset_type: "message",
        purpose: "bidirectional",
      },
    ];

    const { ws, ack } = await connectAndHandshake(ctx.port, { streams });
    await tick();

    const streamId = ack.body.streams[0]?.id;
    expect(streamId).toBeTruthy();
    expect(connectedSession).toBeDefined();

    const received: ReturnType<typeof parseFrame>[] = [];
    ws.on("message", (data, isBinary) => {
      if (isBinary) {
        received.push(parseWsMessage(data, true));
      }
    });

    const payload = new Uint8Array([10, 20, 30]);
    await connectedSession!.sendMedia(streamId!, payload);
    await sleep(80);

    const mediaFrame = received.find((f) => f.kind === "media");
    expect(mediaFrame).toBeDefined();
    if (mediaFrame?.kind === "media") {
      expect(mediaFrame.body).toEqual(payload);
    }

    ws.close();
    await sleep(50);
  });
});

describe("CulpeoServer — graceful close", () => {
  let ctx: TestContext;

  beforeEach(async () => {
    ctx = await createTestContext();
  });

  afterEach(async () => {
    await teardownTestContext(ctx);
  });

  it("calls onDisconnected when the client disconnects", async () => {
    const { ws } = await connectAndHandshake(ctx.port);
    await tick();

    expect(ctx.handler.onConnected).toHaveBeenCalledOnce();

    await new Promise<void>((resolve) => {
      ws.once("close", () => resolve());
      ws.close();
    });

    await sleep(100);

    expect(ctx.handler.onDisconnected).toHaveBeenCalledOnce();
  });

  it("server-initiated close sends culpeo.close and closes the WebSocket", async () => {
    let connectedSession: IServerSession | undefined;
    ctx.handler.onConnected.mockImplementation(async (s) => {
      connectedSession = s;
    });

    const { ws } = await connectAndHandshake(ctx.port);
    await tick();

    const messages: ReturnType<typeof parseFrame>[] = [];
    ws.on("message", (data, isBinary) => {
      messages.push(parseWsMessage(data, isBinary));
    });

    const closedPromise = new Promise<void>((resolve) => {
      ws.once("close", () => resolve());
    });

    await connectedSession!.close("Test close");
    await closedPromise;

    const closeFrame = messages.find(
      (f) => f.kind === "control" && f.event === "culpeo.close",
    );
    expect(closeFrame).toBeDefined();
  });

  it("server close saves the snapshot to the store before calling onDisconnected", async () => {
    const { ws, ack } = await connectAndHandshake(ctx.port);
    const sessionId = ack.headers.sessionId;
    await tick();

    // Disconnect
    await new Promise<void>((resolve) => {
      ws.once("close", () => resolve());
      ws.close();
    });
    await sleep(100);

    const saved = await ctx.store.load(sessionId);
    expect(saved).not.toBeNull();
  });
});

describe("CulpeoServer — maxMessageBytes enforcement", () => {
  let ctx: TestContext;

  beforeEach(async () => {
    ctx = await createTestContext({ maxMessageBytes: 256 });
  });

  afterEach(async () => {
    await teardownTestContext(ctx);
  });

  it("terminates connections that send oversized messages", async () => {
    const { ws } = await connectAndHandshake(ctx.port);
    await tick();

    const closedPromise = new Promise<number>((resolve) => {
      ws.once("close", (code) => resolve(code));
    });

    // Send a message larger than maxMessageBytes (256).
    const bigPayload = "X".repeat(512);
    ws.send(bigPayload);

    const closeCode = await closedPromise;
    // ws closes with 1009 (message too big) when maxPayload is exceeded.
    expect(closeCode).toBe(1009);
  });
});

describe("CulpeoServer — ping/pong keepalive", () => {
  it("server sends culpeo.ping at the configured interval", async () => {
    const ctx = await createTestContext({ pingIntervalMs: 80 });

    try {
      const { ws } = await connectAndHandshake(ctx.port);
      await tick();

      const pings: ReturnType<typeof parseFrame>[] = [];
      ws.on("message", (data, isBinary) => {
        try {
          const frame = parseWsMessage(data, isBinary);
          if (frame.kind === "control" && frame.event === "culpeo.ping") {
            pings.push(frame);
          }
        } catch {
          // ignore parse errors
        }
      });

      // Wait long enough to receive at least one ping (2× interval).
      await sleep(250);

      expect(pings.length).toBeGreaterThanOrEqual(1);

      ws.close();
      await sleep(50);
    } finally {
      await teardownTestContext(ctx);
    }
  });

  it("server processes pong responses without error", async () => {
    const ctx = await createTestContext({ pingIntervalMs: 80 });

    try {
      const { ws } = await connectAndHandshake(ctx.port);
      await tick();

      const serverPingTs: number[] = [];
      ws.on("message", (data, isBinary) => {
        try {
          const frame = parseWsMessage(data, isBinary);
          if (frame.kind === "control" && frame.event === "culpeo.ping") {
            serverPingTs.push((frame.body as { ts: number }).ts);

            // Respond with pong
            const pongFrame = serializeFrame({
              kind: "control",
              event: "culpeo.pong",
              headers: {
                event: "culpeo.pong",
                contentType: "application/json",
              },
              body: {
                ts: (frame.body as { ts: number }).ts,
                server_ts: Date.now() * 1000,
              },
            });
            ws.send(pongFrame.data);
          }
        } catch {
          // ignore
        }
      });

      await sleep(250);

      // At least one ping/pong exchange should have occurred.
      expect(serverPingTs.length).toBeGreaterThanOrEqual(1);

      ws.close();
      await sleep(50);
    } finally {
      await teardownTestContext(ctx);
    }
  });
});

describe("CulpeoServer — multiple concurrent sessions", () => {
  let ctx: TestContext;

  beforeEach(async () => {
    ctx = await createTestContext();
  });

  afterEach(async () => {
    await teardownTestContext(ctx);
  });

  it("handles multiple concurrent sessions independently", async () => {
    const connectedSessions: IServerSession[] = [];
    ctx.handler.onConnected.mockImplementation(async (session) => {
      connectedSessions.push(session);
    });

    // Connect 5 clients simultaneously.
    const connections = await Promise.all(
      Array.from({ length: 5 }, () => connectAndHandshake(ctx.port)),
    );

    await tick();

    expect(connectedSessions).toHaveLength(5);

    // All session IDs should be unique.
    const sessionIds = new Set(connectedSessions.map((s) => s.sessionId));
    expect(sessionIds.size).toBe(5);

    // All init-ack session IDs should also be unique.
    const ackIds = new Set(connections.map((c) => c.ack.headers.sessionId));
    expect(ackIds.size).toBe(5);

    // Close all connections.
    await Promise.all(
      connections.map(
        ({ ws }) =>
          new Promise<void>((resolve) => {
            ws.once("close", () => resolve());
            ws.close();
          }),
      ),
    );

    await sleep(100);
    expect(ctx.handler.onDisconnected).toHaveBeenCalledTimes(5);
  });

  it("isolates media frames to their respective sessions", async () => {
    const mediaPerSession = new Map<string, Uint8Array[]>();
    ctx.handler.onConnected.mockImplementation(async (session) => {
      mediaPerSession.set(session.sessionId, []);
    });
    ctx.handler.onMedia.mockImplementation(async (session, _streamId, data) => {
      mediaPerSession.get(session.sessionId)?.push(data);
    });

    const [c1, c2] = await Promise.all([
      connectAndHandshake(ctx.port),
      connectAndHandshake(ctx.port),
    ]);

    await tick();

    const stream1 = c1.ack.body.streams[0]?.id;
    const stream2 = c2.ack.body.streams[0]?.id;
    expect(stream1).toBeTruthy();
    expect(stream2).toBeTruthy();

    // Send different payloads from each client.
    const payload1 = new Uint8Array([1, 1, 1]);
    const payload2 = new Uint8Array([2, 2, 2]);

    c1.ws.send(
      serializeFrame({
        kind: "media",
        headers: { streamId: stream1!, offset: 0, contentType: "audio/opus" },
        body: payload1,
      }).data,
    );
    c2.ws.send(
      serializeFrame({
        kind: "media",
        headers: { streamId: stream2!, offset: 0, contentType: "audio/opus" },
        body: payload2,
      }).data,
    );

    await tick(40);
    await sleep(80);

    // Each session's media list should contain only its own frames.
    const sess1Id = c1.ack.headers.sessionId;
    const sess2Id = c2.ack.headers.sessionId;
    expect(mediaPerSession.get(sess1Id)).toHaveLength(1);
    expect(mediaPerSession.get(sess2Id)).toHaveLength(1);
    expect(mediaPerSession.get(sess1Id)?.[0]).toEqual(payload1);
    expect(mediaPerSession.get(sess2Id)?.[0]).toEqual(payload2);

    c1.ws.close();
    c2.ws.close();
    await sleep(100);
  });
});

describe("CulpeoServer — InMemorySessionStore", () => {
  it("evicts the oldest session when maxSessions is reached", async () => {
    const store = new InMemorySessionStore({ maxSessions: 2 });

    const makeSnapshot = (id: string) => ({
      sessionId: id,
      version: "0.3" as const,
      bufferWindowMs: 5000,
      streams: [
        {
          id: "s1",
          type: "input" as const,
          content_type: "audio/opus",
          offset_type: "message" as const,
        },
      ],
    });

    await store.save(makeSnapshot("a"));
    await store.save(makeSnapshot("b"));
    await store.save(makeSnapshot("c")); // should evict "a"

    expect(await store.load("a")).toBeNull();
    expect(await store.load("b")).not.toBeNull();
    expect(await store.load("c")).not.toBeNull();
  });

  it("expires sessions after ttlMs", async () => {
    const store = new InMemorySessionStore({ ttlMs: 50 });
    await store.save({
      sessionId: "ttl-test",
      version: "0.3",
      bufferWindowMs: 5000,
      streams: [
        {
          id: "s1",
          type: "input",
          content_type: "audio/opus",
          offset_type: "message",
        },
      ],
    });

    expect(await store.load("ttl-test")).not.toBeNull();

    await sleep(100);

    expect(await store.load("ttl-test")).toBeNull();
  });

  it("deletes sessions explicitly", async () => {
    const store = new InMemorySessionStore();
    await store.save({
      sessionId: "del-test",
      version: "0.3",
      bufferWindowMs: 5000,
      streams: [
        {
          id: "s1",
          type: "input",
          content_type: "audio/opus",
          offset_type: "message",
        },
      ],
    });

    await store.delete("del-test");
    expect(await store.load("del-test")).toBeNull();
  });
});

describe("CulpeoServer — protocol error handling", () => {
  let ctx: TestContext;

  beforeEach(async () => {
    ctx = await createTestContext();
  });

  afterEach(async () => {
    await teardownTestContext(ctx);
  });

  it("sends init-error and closes if first message is not culpeo.init", async () => {
    const port = ctx.port;

    const result = await new Promise<string>((resolve, reject) => {
      const ws = new WebSocket(`ws://127.0.0.1:${port}`);
      ws.once("open", () => {
        // Send something that is not culpeo.init
        const wrongFrame = serializeFrame({
          kind: "control",
          event: "culpeo.ping",
          headers: { event: "culpeo.ping", contentType: "application/json" },
          body: { ts: 0 },
        });
        ws.send(wrongFrame.data);
      });

      ws.on("message", (data, isBinary) => {
        try {
          const frame = parseWsMessage(data, isBinary);
          if (frame.kind === "control" && frame.event === "culpeo.init-error") {
            resolve((frame as InitErrorFrame).headers.code);
          }
        } catch (err) {
          reject(err);
        }
      });

      ws.once("error", reject);
    });

    expect(result).toBe("protocol-error");
  });

  it("rejects unsupported protocol versions with unsupported-version error", async () => {
    const port = ctx.port;

    const errorCode = await new Promise<string>((resolve, reject) => {
      const ws = new WebSocket(`ws://127.0.0.1:${port}`);
      ws.once("open", () => {
        const initFrame = serializeFrame({
          kind: "control",
          event: "culpeo.init",
          headers: {
            event: "culpeo.init",
            authorization: VALID_TOKEN,
            contentType: "application/json",
            bufferWindow: 5000,
          },
          body: {
            version: "99.0", // unsupported
            streams: [DEFAULT_STREAM],
          },
        });
        if (initFrame.frameType !== "text")
          throw new Error("Expected text frame");
        ws.send(initFrame.data);
      });

      ws.on("message", (data, isBinary) => {
        try {
          const frame = parseWsMessage(data, isBinary);
          if (frame.kind === "control" && frame.event === "culpeo.init-error") {
            resolve((frame as InitErrorFrame).headers.code);
          }
        } catch (err) {
          reject(err);
        }
      });

      ws.once("error", reject);
    });

    expect(errorCode).toBe("unsupported-version");
  });
});

describe("CulpeoServer — createCulpeoServer factory", () => {
  it("returns a CulpeoServer instance", async () => {
    const store = new InMemorySessionStore();
    const server = createCulpeoServer({
      authenticate: async () => true,
      handler: makeNoOpHandler(),
      sessionStore: store,
    });
    expect(server).toBeDefined();
    expect(typeof server.attach).toBe("function");
    expect(typeof server.listen).toBe("function");
    expect(typeof server.close).toBe("function");
  });

  it("listen() creates a standalone server on the given port", async () => {
    const port = await getFreePort();
    const server = createCulpeoServer({
      authenticate: async () => true,
      handler: makeNoOpHandler(),
    });

    await server.listen(port);

    // Verify we can connect to it.
    const { ws, ack } = await connectAndHandshake(port);
    expect(ack.event).toBe("culpeo.init-ack");

    ws.close();
    await sleep(50);
    await server.close();
  });
});

// ---------------------------------------------------------------------------
// TS-P3-002 — Handler errors surface via onError, session stays alive
// ---------------------------------------------------------------------------

describe("CulpeoServer — handler error routing (TS-P3-002)", () => {
  let ctx: TestContext;

  beforeEach(async () => {
    ctx = await createTestContext();
  });

  afterEach(async () => {
    await teardownTestContext(ctx);
  });

  it("calls handler.onError when onMedia throws, session stays alive", async () => {
    const testError = new Error("deliberate media handler error");
    ctx.handler.onMedia.mockRejectedValueOnce(testError);

    const onErrorCalls: Array<{ session: IServerSession; error: unknown }> = [];
    (ctx.handler as ICulpeoStreamHandler).onError = vi.fn(
      async (session: IServerSession, error: unknown) => {
        onErrorCalls.push({ session, error });
      },
    );

    const { ws, ack } = await connectAndHandshake(ctx.port);
    await tick();

    const streamId = ack.body.streams[0]?.id;
    expect(streamId).toBeTruthy();

    const payload = new Uint8Array([1, 2, 3]);
    ws.send(
      serializeFrame({
        kind: "media",
        headers: { streamId: streamId!, offset: 0, contentType: "audio/opus" },
        body: payload,
      }).data,
    );
    await sleep(80);

    // onError must have been called with the thrown error.
    expect(onErrorCalls).toHaveLength(1);
    expect(onErrorCalls[0]?.error).toBe(testError);

    // Session must still be alive — send a second media frame successfully.
    ctx.handler.onMedia.mockResolvedValueOnce(undefined);
    ws.send(
      serializeFrame({
        kind: "media",
        headers: { streamId: streamId!, offset: 1, contentType: "audio/opus" },
        body: payload,
      }).data,
    );
    await sleep(80);

    // WebSocket should still be open.
    expect(ws.readyState).toBe(WebSocket.OPEN);

    ws.close();
    await sleep(50);
  });
});

// ---------------------------------------------------------------------------
// TS-P3-003 — LRU eviction in InMemorySessionStore
// ---------------------------------------------------------------------------

describe("InMemorySessionStore — LRU eviction (TS-P3-003)", () => {
  const makeSnapshot = (id: string) => ({
    sessionId: id,
    version: "0.3" as const,
    bufferWindowMs: 5000,
    streams: [
      {
        id: "s1",
        type: "input" as const,
        content_type: "audio/opus",
        offset_type: "message" as const,
      },
    ],
  });

  it("evicts the LRU session (not the oldest inserted) when at capacity", async () => {
    const store = new InMemorySessionStore({ maxSessions: 2 });

    await store.save(makeSnapshot("a"));
    await store.save(makeSnapshot("b"));

    // Access "a" so it becomes more-recently-used than "b".
    await store.load("a");

    // Saving "c" should evict "b" (LRU), not "a" (recently accessed).
    await store.save(makeSnapshot("c"));

    expect(await store.load("a")).not.toBeNull(); // a was recently accessed — kept
    expect(await store.load("b")).toBeNull(); // b was LRU — evicted
    expect(await store.load("c")).not.toBeNull(); // c is new — kept
  });
});

// ---------------------------------------------------------------------------
// SEC-020 — authenticate receives sessionId on resumption
// ---------------------------------------------------------------------------

describe("CulpeoServer — authenticate receives sessionId on resumption (SEC-020)", () => {
  let ctx: TestContext;

  beforeEach(async () => {
    ctx = await createTestContext({ pingIntervalMs: 0 });
  });

  afterEach(async () => {
    await teardownTestContext(ctx);
  });

  it("passes the session ID to authenticate when the client resumes", async () => {
    // First connection — establish and record the session ID.
    const { ws: ws1, ack: ack1 } = await connectAndHandshake(ctx.port);
    const sessionId = ack1.headers.sessionId;
    await tick();

    await new Promise<void>((resolve) => {
      ws1.once("close", () => resolve());
      ws1.close();
    });
    await sleep(50);

    // Clear the call history so the second call is unambiguous.
    ctx.authenticate.mockClear();

    // Second connection — resume with the stored session ID.
    const { ws: ws2 } = await connectAndHandshake(ctx.port, { sessionId });

    // authenticate should have been called with (token, sessionId).
    expect(ctx.authenticate).toHaveBeenCalledWith(VALID_TOKEN, sessionId);

    ws2.close();
    await sleep(50);
  });
});
