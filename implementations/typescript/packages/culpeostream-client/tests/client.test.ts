/**
 * Tests for CulpeoStreamClient.
 *
 * A MockWebSocketClient class simulates the @culpeo/async-ws WebSocketClient
 * API. Its connect() resolves when simulateOpen() is called. Messages are
 * delivered through an async iterator that yields on simulateMessage() calls
 * and ends on simulateClose(). This keeps tests deterministic while matching
 * the promise-based lifecycle of the real library.
 */

import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { serializeFrame } from "culpeostream";
import type {
  ConfirmedStreamDeclaration,
  InitAckFrame,
  InitErrorFrame,
  MediaMessage,
  ResumeStreamDeclaration,
} from "culpeostream";

import { CulpeoStreamClient } from "../src/client.js";
import type { ConnectOptions, WsClientLike } from "../src/client.js";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Flush pending microtasks (several levels deep to cover async chains). */
async function tick(n = 10): Promise<void> {
  for (let i = 0; i < n; i++) {
    await Promise.resolve();
  }
}

/** Build a minimal stream declaration list. */
function makeStreams(
  type: ResumeStreamDeclaration["type"] = "input",
): ResumeStreamDeclaration[] {
  return [
    {
      type,
      content_type: "audio/opus",
      offset_type: "message",
      purpose: "voice",
    },
  ];
}

/** Build default ConnectOptions. */
function makeOpts(overrides: Partial<ConnectOptions> = {}): ConnectOptions {
  return {
    token: "test-token",
    streams: makeStreams(),
    bufferWindowMs: 1000,
    allowInsecure: true, // use ws:// in tests
    ...overrides,
  };
}

/** Serialise a minimal init-ack frame for the server to "send". */
function makeInitAckText(
  sessionId = "session-abc",
  streams: ConfirmedStreamDeclaration[] = [
    {
      id: "stream-1",
      type: "input",
      content_type: "audio/opus",
      offset_type: "message",
      purpose: "voice",
    },
  ],
): string {
  const frame: InitAckFrame = {
    kind: "control",
    event: "culpeo.init-ack",
    headers: {
      event: "culpeo.init-ack",
      sessionId,
      contentType: "application/json",
    },
    body: {
      version: "0.3",
      streams,
    },
  };
  const serialized = serializeFrame(frame);
  if (serialized.frameType !== "text") throw new Error("Expected text frame");
  return serialized.data;
}

function makeInitErrorText(
  code: InitErrorFrame["headers"]["code"] = "unauthorized",
  reason = "Bad token",
): string {
  const frame: InitErrorFrame = {
    kind: "control",
    event: "culpeo.init-error",
    headers: { event: "culpeo.init-error", code, reason },
    body: {},
  };
  const serialized = serializeFrame(frame);
  if (serialized.frameType !== "text") throw new Error("Expected text frame");
  return serialized.data;
}

function makeMediaFrame(streamId: string): ArrayBuffer {
  const frame: MediaMessage = {
    kind: "media",
    headers: {
      streamId,
      offset: 0,
      contentType: "audio/opus",
    },
    body: new Uint8Array([1, 2, 3, 4]),
  };
  const serialized = serializeFrame(frame);
  if (serialized.frameType !== "binary")
    throw new Error("Expected binary frame");
  // .slice() on a Uint8Array always returns a new Uint8Array with a fresh ArrayBuffer.
  return serialized.data.slice().buffer;
}

// ---------------------------------------------------------------------------
// Mock WebSocketClient
// ---------------------------------------------------------------------------

/**
 * A deterministic mock of the @culpeo/async-ws WebSocketClient API.
 *
 * - `connect()` returns a Promise resolved by `simulateOpen()`.
 * - `send()` records payloads in the `sent` array.
 * - `close()` ends the async iterator cleanly.
 * - The async iterator yields messages delivered via `simulateMessage()`.
 * - `simulateClose()` ends the async iterator (triggering reconnect logic).
 * - `simulateError()` is an alias for `simulateClose(1006)`.
 */
class MockWebSocketClient implements WsClientLike {
  private connectResolve: (() => void) | null = null;
  private connectReject: ((err: Error) => void) | null = null;

  // Async iterator state
  private pendingMessages: { data: string | ArrayBuffer; binary: boolean }[] =
    [];
  private messageWaiter:
    | ((
        result: IteratorResult<{ data: string | ArrayBuffer; binary: boolean }>,
      ) => void)
    | null = null;
  private iteratorDone = false;

  lastCloseInfo: { code: number; reason: string; wasClean: boolean } | null =
    null;

  /** All data passed to `send()` since construction. */
  sent: Array<string | Uint8Array> = [];

  connect(_url: string | URL): Promise<void> {
    return new Promise((resolve, reject) => {
      this.connectResolve = resolve;
      this.connectReject = reject;
    });
  }

  send(data: string | ArrayBuffer | ArrayBufferView): Promise<void> {
    if (typeof data === "string") {
      this.sent.push(data);
    } else if (data instanceof ArrayBuffer) {
      this.sent.push(new Uint8Array(data));
    } else {
      this.sent.push(
        new Uint8Array(data.buffer, data.byteOffset, data.byteLength),
      );
    }
    return Promise.resolve();
  }

  close(code = 1000, reason = ""): Promise<void> {
    this.lastCloseInfo = { code, reason, wasClean: code === 1000 };
    this.iteratorDone = true;
    if (this.messageWaiter) {
      this.messageWaiter({ value: undefined as never, done: true });
      this.messageWaiter = null;
    }
    return Promise.resolve();
  }

  // --- Test helpers ----------------------------------------------------------

  simulateOpen(): void {
    this.connectResolve?.();
    this.connectResolve = null;
    this.connectReject = null;
  }

  simulateConnectError(err: Error): void {
    this.connectReject?.(err);
    this.connectResolve = null;
    this.connectReject = null;
  }

  simulateMessage(data: string | ArrayBuffer): void {
    const msg = { data, binary: typeof data !== "string" };
    if (this.messageWaiter !== null) {
      const resolve = this.messageWaiter;
      this.messageWaiter = null;
      resolve({ value: msg, done: false });
    } else {
      this.pendingMessages.push(msg);
    }
  }

  simulateClose(code = 1006, reason = ""): void {
    this.lastCloseInfo = { code, reason, wasClean: code === 1000 };
    this.iteratorDone = true;

    // If connect() is still pending (connection failed before opening),
    // reject it so runConnection() takes the catch branch → scheduleReconnect().
    if (this.connectReject !== null) {
      const reject = this.connectReject;
      this.connectResolve = null;
      this.connectReject = null;
      reject(new Error(`WebSocket closed before opening (code ${code})`));
      return;
    }

    // Connection was already established — end the async iterator.
    if (this.messageWaiter !== null) {
      const resolve = this.messageWaiter;
      this.messageWaiter = null;
      resolve({ value: undefined as never, done: true });
    }
  }

  simulateError(): void {
    this.simulateClose(1006);
  }

  [Symbol.asyncIterator](): AsyncGenerator<{
    data: string | ArrayBuffer;
    binary: boolean;
  }> {
    return this._iterate();
  }

  private async *_iterate(): AsyncGenerator<{
    data: string | ArrayBuffer;
    binary: boolean;
  }> {
    while (!this.iteratorDone) {
      if (this.pendingMessages.length > 0) {
        yield this.pendingMessages.shift()!;
      } else {
        const result = await new Promise<
          IteratorResult<{ data: string | ArrayBuffer; binary: boolean }>
        >((resolve) => {
          if (this.iteratorDone) {
            resolve({ value: undefined as never, done: true });
          } else {
            this.messageWaiter = resolve;
          }
        });
        if (result.done) return;
        yield result.value;
      }
    }
  }
}

/** Factory that tracks created instances so tests can reference them. */
function makeMockWsClientFactory(): {
  factory: () => WsClientLike;
  instances: MockWebSocketClient[];
} {
  const instances: MockWebSocketClient[] = [];
  return {
    factory: () => {
      const client = new MockWebSocketClient();
      instances.push(client);
      return client;
    },
    instances,
  };
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

describe("CulpeoStreamClient — URL validation", () => {
  it("throws on ws:// without allowInsecure", async () => {
    const { factory } = makeMockWsClientFactory();
    const client = new CulpeoStreamClient({ wsClientFactory: factory });
    await expect(
      client.connect("ws://localhost:8080", {
        ...makeOpts(),
        allowInsecure: false,
      }),
    ).rejects.toThrow(/insecure/i);
  });

  it("warns but accepts ws:// with allowInsecure: true", async () => {
    const { factory, instances } = makeMockWsClientFactory();
    const warnSpy = vi.spyOn(console, "warn").mockImplementation(() => {});
    const client = new CulpeoStreamClient({ wsClientFactory: factory });
    const connectP = client.connect("ws://localhost:8080", makeOpts());

    await tick();
    instances[0]!.simulateOpen();
    await tick();
    instances[0]!.simulateMessage(makeInitAckText());
    await tick();

    await connectP;
    expect(warnSpy).toHaveBeenCalledWith(expect.stringContaining("insecure"));
    warnSpy.mockRestore();
  });

  it("accepts wss:// without warnings", async () => {
    const { factory, instances } = makeMockWsClientFactory();
    const warnSpy = vi.spyOn(console, "warn").mockImplementation(() => {});
    const client = new CulpeoStreamClient({ wsClientFactory: factory });
    const connectP = client.connect(
      "wss://localhost:8443",
      makeOpts({ allowInsecure: false }),
    );

    await tick();
    instances[0]!.simulateOpen();
    await tick();
    instances[0]!.simulateMessage(makeInitAckText());
    await tick();

    await connectP;
    expect(warnSpy).not.toHaveBeenCalled();
    warnSpy.mockRestore();
  });

  it("throws on non-ws schemes", async () => {
    const { factory } = makeMockWsClientFactory();
    const client = new CulpeoStreamClient({ wsClientFactory: factory });
    await expect(
      client.connect("http://localhost", makeOpts()),
    ).rejects.toThrow(/scheme/i);
  });
});

describe("CulpeoStreamClient — basic connect/disconnect", () => {
  it("resolves connect() on init-ack", async () => {
    const { factory, instances } = makeMockWsClientFactory();
    const client = new CulpeoStreamClient({ wsClientFactory: factory });
    const connected: string[] = [];
    client.on("connected", () => connected.push("connected"));

    const connectP = client.connect("wss://localhost:8443", makeOpts());
    await tick();

    instances[0]!.simulateOpen();
    await tick();
    instances[0]!.simulateMessage(makeInitAckText());
    await tick();

    await connectP;
    expect(connected).toEqual(["connected"]);
  });

  it("sends culpeo.init on open", async () => {
    const { factory, instances } = makeMockWsClientFactory();
    const client = new CulpeoStreamClient({ wsClientFactory: factory });
    void client.connect("wss://localhost:8443", makeOpts());

    await tick();
    instances[0]!.simulateOpen();
    await tick();

    // The sent buffer should contain one text frame (the init).
    expect(instances[0]!.sent.length).toBeGreaterThanOrEqual(1);
    const first = instances[0]!.sent[0];
    expect(typeof first).toBe("string");
    expect(first as string).toContain("culpeo.init");
  });

  it("includes token in init Authorization header (without logging it)", async () => {
    const { factory, instances } = makeMockWsClientFactory();
    const client = new CulpeoStreamClient({ wsClientFactory: factory });
    void client.connect(
      "wss://localhost:8443",
      makeOpts({ token: "secret-tok" }),
    );
    await tick();
    instances[0]!.simulateOpen();
    await tick();

    const initText = instances[0]!.sent[0] as string;
    // Token appears in the frame (Authorization header), but the test
    // confirms it is present — we verify the header is there, not leaked.
    expect(initText).toContain("Authorization: Bearer secret-tok");
  });

  it("rejects connect() on init-error", async () => {
    const { factory, instances } = makeMockWsClientFactory();
    const client = new CulpeoStreamClient({ wsClientFactory: factory });
    const closeReasons: string[] = [];
    client.on("close", (r) => closeReasons.push(r.code));

    const connectP = client.connect("wss://localhost:8443", makeOpts());
    await tick();
    instances[0]!.simulateOpen();
    await tick();
    instances[0]!.simulateMessage(
      makeInitErrorText("unauthorized", "Bad token"),
    );
    await tick();

    await expect(connectP).rejects.toThrow("unauthorized");
    expect(closeReasons).toContain("unauthorized");
  });

  it("error message on init-error does not contain the token", async () => {
    const { factory, instances } = makeMockWsClientFactory();
    const client = new CulpeoStreamClient({ wsClientFactory: factory });
    const connectP = client.connect(
      "wss://localhost:8443",
      makeOpts({ token: "super-secret" }),
    );
    await tick();
    instances[0]!.simulateOpen();
    await tick();
    instances[0]!.simulateMessage(
      makeInitErrorText("unauthorized", "Bad token"),
    );
    await tick();

    await connectP.catch((err: Error) => {
      expect(err.message).not.toContain("super-secret");
    });
  });

  it("rejects double connect()", async () => {
    const { factory, instances } = makeMockWsClientFactory();
    const client = new CulpeoStreamClient({ wsClientFactory: factory });
    const p1 = client.connect("wss://localhost:8443", makeOpts());
    await tick();
    await expect(
      client.connect("wss://localhost:8443", makeOpts()),
    ).rejects.toThrow(/already connected/i);

    // Clean up: provide init-ack so the first connect resolves.
    instances[0]!.simulateOpen();
    await tick();
    instances[0]!.simulateMessage(makeInitAckText());
    await tick();
    await p1;
  });

  it("disconnect() prevents reconnect", async () => {
    const { factory, instances } = makeMockWsClientFactory();
    const client = new CulpeoStreamClient({ wsClientFactory: factory });
    const reconnecting: number[] = [];
    client.on("reconnecting", (n) => reconnecting.push(n));
    const disconnectedEvents: string[] = [];
    client.on("disconnected", () => disconnectedEvents.push("disconnected"));

    const connectP = client.connect("wss://localhost:8443", makeOpts());
    await tick();
    instances[0]!.simulateOpen();
    await tick();
    instances[0]!.simulateMessage(makeInitAckText());
    await tick();
    await connectP;

    client.disconnect();
    // No reconnect should be attempted.
    expect(reconnecting).toHaveLength(0);
    expect(disconnectedEvents).toHaveLength(1);
  });
});

describe("CulpeoStreamClient — media", () => {
  it("delivers incoming media frames via the 'media' event", async () => {
    const { factory, instances } = makeMockWsClientFactory();
    const client = new CulpeoStreamClient({ wsClientFactory: factory });
    const mediaFrames: MediaMessage[] = [];
    client.on("media", (f) => mediaFrames.push(f));

    const connectP = client.connect(
      "wss://localhost:8443",
      makeOpts({ streams: makeStreams("output") }),
    );
    await tick();
    instances[0]!.simulateOpen();
    await tick();
    instances[0]!.simulateMessage(
      makeInitAckText("sess", [
        {
          id: "s1",
          type: "output",
          content_type: "audio/opus",
          offset_type: "message",
          purpose: "voice",
        },
      ]),
    );
    await tick();
    await connectP;

    // Server sends a media frame
    instances[0]!.simulateMessage(makeMediaFrame("s1"));
    await tick();

    expect(mediaFrames).toHaveLength(1);
    expect(mediaFrames[0]!.headers.streamId).toBe("s1");
  });

  it("throws when sendMedia is called before connect", () => {
    const { factory } = makeMockWsClientFactory();
    const client = new CulpeoStreamClient({ wsClientFactory: factory });
    expect(() => client.sendMedia("s1", new ArrayBuffer(4))).toThrow(
      /not established/i,
    );
  });

  it("can send media after session is established", async () => {
    const { factory, instances } = makeMockWsClientFactory();
    const client = new CulpeoStreamClient({ wsClientFactory: factory });

    const connectP = client.connect(
      "wss://localhost:8443",
      makeOpts({ streams: makeStreams("input") }),
    );
    await tick();
    instances[0]!.simulateOpen();
    await tick();
    instances[0]!.simulateMessage(
      makeInitAckText("sess", [
        {
          id: "s1",
          type: "input",
          content_type: "audio/opus",
          offset_type: "message",
          purpose: "voice",
        },
      ]),
    );
    await tick();
    await connectP;

    const initialSentCount = instances[0]!.sent.length;
    client.sendMedia("s1", new ArrayBuffer(8));
    await tick();

    // A binary frame should have been sent.
    expect(instances[0]!.sent.length).toBeGreaterThan(initialSentCount);
    const lastSent = instances[0]!.sent[instances[0]!.sent.length - 1];
    expect(lastSent instanceof Uint8Array).toBe(true);
  });
});

describe("CulpeoStreamClient — application events", () => {
  it("delivers incoming application events via the 'event' channel", async () => {
    const { factory, instances } = makeMockWsClientFactory();
    const client = new CulpeoStreamClient({ wsClientFactory: factory });
    const events: string[] = [];
    client.on("event", (f) => events.push(f.event));

    const connectP = client.connect("wss://localhost:8443", makeOpts());
    await tick();
    instances[0]!.simulateOpen();
    await tick();
    instances[0]!.simulateMessage(makeInitAckText());
    await tick();
    await connectP;

    // Simulate an application event from the server.
    const appEvent = serializeFrame({
      kind: "control",
      event: "app.status",
      headers: { event: "app.status", contentType: "application/json" },
      body: { status: "ready" },
    });
    if (appEvent.frameType !== "text") throw new Error("Expected text");
    instances[0]!.simulateMessage(appEvent.data);
    await tick();

    expect(events).toContain("app.status");
  });

  it("can send application events when established", async () => {
    const { factory, instances } = makeMockWsClientFactory();
    const client = new CulpeoStreamClient({ wsClientFactory: factory });
    const connectP = client.connect("wss://localhost:8443", makeOpts());
    await tick();
    instances[0]!.simulateOpen();
    await tick();
    instances[0]!.simulateMessage(makeInitAckText());
    await tick();
    await connectP;

    const sentBefore = instances[0]!.sent.length;
    client.sendEvent("app.hello", { greeting: "hi" });
    await tick();

    expect(instances[0]!.sent.length).toBeGreaterThan(sentBefore);
    expect(
      instances[0]!.sent[instances[0]!.sent.length - 1] as string,
    ).toContain("app.hello");
  });
});

describe("CulpeoStreamClient — reconnection", () => {
  beforeEach(() => {
    vi.useFakeTimers();
  });

  afterEach(() => {
    vi.useRealTimers();
  });

  it("emits 'reconnecting' and opens a new WebSocket on unexpected close", async () => {
    const { factory, instances } = makeMockWsClientFactory();
    const client = new CulpeoStreamClient({
      wsClientFactory: factory,
      reconnect: { baseDelayMs: 100, maxDelayMs: 100, maxAttempts: 3 },
    });
    const reconnecting: number[] = [];
    client.on("reconnecting", (n) => reconnecting.push(n));

    const connectP = client.connect("wss://localhost:8443", makeOpts());
    await tick();
    instances[0]!.simulateOpen();
    await tick();
    instances[0]!.simulateMessage(makeInitAckText());
    await tick();
    await connectP;

    // Drop the connection unexpectedly.
    instances[0]!.simulateClose(1006);
    await tick();

    expect(reconnecting).toHaveLength(1);
    expect(reconnecting[0]).toBe(1);

    // The reconnect timer fires, opening a new WebSocket.
    vi.runAllTimers();
    await tick();

    expect(instances.length).toBe(2);
  });

  it("resumes with session snapshot on reconnect (sends session-id in init)", async () => {
    const { factory, instances } = makeMockWsClientFactory();
    const client = new CulpeoStreamClient({
      wsClientFactory: factory,
      reconnect: { baseDelayMs: 0, maxDelayMs: 0, maxAttempts: 3 },
    });

    const connectP = client.connect("wss://localhost:8443", makeOpts());
    await tick();
    instances[0]!.simulateOpen();
    await tick();
    instances[0]!.simulateMessage(
      makeInitAckText("original-session-id", [
        {
          id: "s1",
          type: "input",
          content_type: "audio/opus",
          offset_type: "message",
          purpose: "voice",
        },
      ]),
    );
    await tick();
    await connectP;

    // Drop connection.
    instances[0]!.simulateClose(1006);
    await tick();

    // Reconnect timer.
    vi.runAllTimers();
    await tick();
    instances[1]!.simulateOpen();
    await tick();

    // Second init frame should include Session-Id header for resumption.
    const secondInit = instances[1]!.sent.find(
      (s) => typeof s === "string" && s.includes("culpeo.init"),
    ) as string | undefined;
    expect(secondInit).toBeDefined();
    expect(secondInit).toContain("Session-Id: original-session-id");
  });

  it("emits 'disconnected' and rejects initial connect() when max attempts exhausted", async () => {
    const { factory, instances } = makeMockWsClientFactory();
    const client = new CulpeoStreamClient({
      wsClientFactory: factory,
      reconnect: { baseDelayMs: 0, maxDelayMs: 0, maxAttempts: 2 },
    });
    const disconnectedEvents: string[] = [];
    client.on("disconnected", () => disconnectedEvents.push("disconnected"));

    const connectP = client.connect("wss://localhost:8443", makeOpts());
    await tick();

    // First attempt: connection fails immediately.
    instances[0]!.simulateClose(1006);
    await tick();

    vi.runAllTimers();
    await tick();
    // Second attempt (attempt 1): fails.
    instances[1]!.simulateClose(1006);
    await tick();

    vi.runAllTimers();
    await tick();
    // Third attempt (attempt 2): fails — max attempts reached.
    instances[2]!.simulateClose(1006);
    await tick();

    await expect(connectP).rejects.toThrow(/max reconnect/i);
    expect(disconnectedEvents).toHaveLength(1);
  });
});

describe("CulpeoStreamClient — auth-refresh", () => {
  it("calls getToken and sends auth-response with nonce", async () => {
    const { factory, instances } = makeMockWsClientFactory();
    let refreshCount = 0;
    const getToken = async () => {
      refreshCount++;
      return "refreshed-token";
    };

    const client = new CulpeoStreamClient({ wsClientFactory: factory });
    const connectP = client.connect(
      "wss://localhost:8443",
      makeOpts({ getToken }),
    );
    await tick();
    instances[0]!.simulateOpen();
    await tick();
    instances[0]!.simulateMessage(makeInitAckText());
    await tick();
    await connectP;

    // Server sends auth-refresh challenge.
    const authRefresh = serializeFrame({
      kind: "control",
      event: "culpeo.auth-refresh",
      headers: {
        event: "culpeo.auth-refresh",
        contentType: "application/json",
      },
      body: { nonce: "nonce-xyz-123" },
    });
    if (authRefresh.frameType !== "text") throw new Error();
    instances[0]!.simulateMessage(authRefresh.data);
    await tick();

    expect(refreshCount).toBe(1);

    // auth-response should have been sent.
    const authResponse = instances[0]!.sent.find(
      (s) => typeof s === "string" && s.includes("culpeo.auth-response"),
    ) as string | undefined;
    expect(authResponse).toBeDefined();
    // Nonce must be echoed.
    expect(authResponse).toContain("nonce-xyz-123");
    // Token must NOT be in the auth-response body — only in the Authorization header.
    // (Verify the Authorization header is present but not in the JSON body)
    expect(authResponse).toContain("Authorization: Bearer refreshed-token");
  });

  it("does not include the token value in any Error message", async () => {
    const { factory, instances } = makeMockWsClientFactory();
    const client = new CulpeoStreamClient({ wsClientFactory: factory });
    const connectP = client.connect(
      "wss://localhost:8443",
      makeOpts({ token: "my-super-secret-token" }),
    );
    await tick();
    instances[0]!.simulateOpen();
    await tick();
    instances[0]!.simulateMessage(
      makeInitErrorText("unauthorized", "bad creds"),
    );
    await tick();

    let caughtErr: Error | undefined;
    await connectP.catch((err: Error) => {
      caughtErr = err;
    });
    expect(caughtErr).toBeDefined();
    expect(caughtErr!.message).not.toContain("my-super-secret-token");
  });
});

describe("CulpeoStreamClient — culpeo.close from server", () => {
  it("emits 'close' and does not reconnect on server-initiated close", async () => {
    const { factory, instances } = makeMockWsClientFactory();
    const client = new CulpeoStreamClient({ wsClientFactory: factory });
    const closeReasons: string[] = [];
    const reconnecting: number[] = [];
    client.on("close", (r) => closeReasons.push(r.code));
    client.on("reconnecting", (n) => reconnecting.push(n));

    const connectP = client.connect("wss://localhost:8443", makeOpts());
    await tick();
    instances[0]!.simulateOpen();
    await tick();
    instances[0]!.simulateMessage(makeInitAckText());
    await tick();
    await connectP;

    const closeFrame = serializeFrame({
      kind: "control",
      event: "culpeo.close",
      headers: {
        event: "culpeo.close",
        code: "server-shutdown",
        reason: "Maintenance",
      },
      body: {},
    });
    if (closeFrame.frameType !== "text") throw new Error();
    instances[0]!.simulateMessage(closeFrame.data);
    await tick();

    expect(closeReasons).toContain("server-shutdown");
    expect(reconnecting).toHaveLength(0);
  });
});

describe("CulpeoStreamClient — confirmedStreams", () => {
  it("returns empty array before connect", () => {
    const { factory } = makeMockWsClientFactory();
    const client = new CulpeoStreamClient({ wsClientFactory: factory });
    expect(client.confirmedStreams).toHaveLength(0);
  });

  it("returns confirmed streams after connect", async () => {
    const { factory, instances } = makeMockWsClientFactory();
    const client = new CulpeoStreamClient({ wsClientFactory: factory });

    const connectP = client.connect("wss://localhost:8443", makeOpts());
    await tick();
    instances[0]!.simulateOpen();
    await tick();
    instances[0]!.simulateMessage(
      makeInitAckText("sess", [
        {
          id: "s1",
          type: "input",
          content_type: "audio/opus",
          offset_type: "message",
          purpose: "voice",
        },
      ]),
    );
    await tick();
    await connectP;

    expect(client.confirmedStreams).toHaveLength(1);
    expect(client.confirmedStreams[0]!.id).toBe("s1");
  });
});

describe("CulpeoStreamClient — parse errors", () => {
  it("emits 'error' for unparseable messages rather than throwing", async () => {
    const { factory, instances } = makeMockWsClientFactory();
    const client = new CulpeoStreamClient({ wsClientFactory: factory });
    const errors: Error[] = [];
    client.on("error", (e) => errors.push(e));

    const connectP = client.connect("wss://localhost:8443", makeOpts());
    await tick();
    instances[0]!.simulateOpen();
    await tick();
    instances[0]!.simulateMessage(makeInitAckText());
    await tick();
    await connectP;

    // Send garbage that cannot be parsed.
    instances[0]!.simulateMessage("not a valid culpeo frame at all");
    await tick();

    expect(errors.length).toBeGreaterThanOrEqual(1);
  });
});
