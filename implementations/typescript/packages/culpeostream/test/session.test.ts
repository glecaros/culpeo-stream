import { describe, expect, it } from "vitest";

import { CulpeoClientSession, CulpeoServerSession } from "../src/session.js";
import type { CulpeoFrame } from "../src/types.js";

async function establishServer(
  server: CulpeoServerSession,
  outbound: CulpeoFrame[],
): Promise<void> {
  await server.receive({
    kind: "control",
    event: "culpeo.init",
    headers: {
      event: "culpeo.init",
      authorization: "Bearer token",
      contentType: "application/json",
      bufferWindow: 1_000,
    },
    body: {
      version: "0.3",
      streams: [{ content_type: "audio/opus", type: "output" }],
    },
  });
  expect(outbound[0]).toMatchObject({ event: "culpeo.init-ack" });
}

describe("session state machine", () => {
  it("rejects frames before init on the server", async () => {
    const outbound: CulpeoFrame[] = [];
    const server = new CulpeoServerSession({
      sendFrame: (frame) => {
        outbound.push(frame);
      },
    });

    await expect(
      server.receive({
        kind: "control",
        event: "culpeo.ping",
        headers: { event: "culpeo.ping", contentType: "application/json" },
        body: { ts: 1 },
      }),
    ).rejects.toThrow(/first frame must be culpeo.init/i);

    expect(server.state).toBe("closed");
    expect(outbound[0]).toMatchObject({ event: "culpeo.close" });
  });

  it("establishes a client/server session and snapshots offsets for resumption", async () => {
    const clientOutbound: CulpeoFrame[] = [];
    const serverOutbound: CulpeoFrame[] = [];

    const client = new CulpeoClientSession({
      streams: [
        {
          content_type: "audio/pcm;rate=16000;channels=1;bits=16",
          type: "input",
          purpose: "voice",
        },
        { content_type: "audio/opus", type: "output", purpose: "assistant" },
      ],
      sendFrame: (frame) => {
        clientOutbound.push(frame);
      },
    });
    const server = new CulpeoServerSession({
      sendFrame: (frame) => {
        serverOutbound.push(frame);
      },
      generateId: (() => {
        const ids = ["session-1", "stream-1", "stream-2", "nonce-1"];
        return () => ids.shift() ?? "fallback-id";
      })(),
    });

    await client.start({
      authorization: "Bearer token",
      bufferWindowMs: 7_500,
    });
    await server.receive(clientOutbound[0]!);
    await client.receive(serverOutbound[0]!);

    expect(client.state).toBe("established");
    expect(server.state).toBe("established");

    await client.sendMedia("stream-1", new Uint8Array(640), 100);
    await server.receive(clientOutbound[1]!);
    await server.sendMedia("stream-2", new Uint8Array([1, 2, 3]), 200);
    await client.receive(serverOutbound[1]!);

    const snapshot = client.createSnapshot();
    expect(snapshot.sessionId).toBe("session-1");
    expect(snapshot.disconnectedAtMs).toEqual(expect.any(Number));
    expect(snapshot.streams).toEqual([
      {
        id: "stream-1",
        content_type: "audio/pcm;rate=16000;channels=1;bits=16",
        type: "input",
        purpose: "voice",
        resume_offset: 0,
      },
      {
        id: "stream-2",
        content_type: "audio/opus",
        type: "output",
        purpose: "assistant",
        resume_offset: 0,
      },
    ]);

    const resumeClientOutbound: CulpeoFrame[] = [];
    const resumeServerOutbound: CulpeoFrame[] = [];
    const resumeClient = new CulpeoClientSession({
      streams: [
        {
          content_type: "audio/pcm;rate=16000;channels=1;bits=16",
          type: "input",
          purpose: "voice",
        },
        { content_type: "audio/opus", type: "output", purpose: "assistant" },
      ],
      sendFrame: (frame) => {
        resumeClientOutbound.push(frame);
      },
    });
    const resumeServer = new CulpeoServerSession({
      sendFrame: (frame) => {
        resumeServerOutbound.push(frame);
      },
      resumeSnapshot: snapshot,
      generateId: (() => {
        const ids = ["session-1"];
        return () => ids.shift() ?? "fallback-id";
      })(),
    });

    await resumeClient.start({
      authorization: "Bearer token-2",
      bufferWindowMs: 7_500,
      resumeFrom: snapshot,
    });
    expect(resumeClientOutbound[0]).toMatchObject({
      event: "culpeo.init",
      headers: { sessionId: "session-1" },
      body: {
        streams: [
          { id: "stream-1", resume_offset: 0 },
          { id: "stream-2", resume_offset: 0 },
        ],
      },
    });

    await resumeServer.receive(resumeClientOutbound[0]!);
    await resumeClient.receive(resumeServerOutbound[0]!);

    expect(resumeClient.state).toBe("established");
    expect(resumeServerOutbound[0]).toMatchObject({
      event: "culpeo.init-ack",
      body: {
        streams: [
          { id: "stream-1", resume_offset: 0 },
          { id: "stream-2", resume_offset: 0 },
        ],
      },
    });
  });

  it("surfaces unsupported versions from init-error", async () => {
    const outbound: CulpeoFrame[] = [];
    const client = new CulpeoClientSession({
      streams: [{ content_type: "audio/opus", type: "output" }],
      sendFrame: (frame) => {
        outbound.push(frame);
      },
      version: "9.9",
    });
    const serverOutbound: CulpeoFrame[] = [];
    const server = new CulpeoServerSession({
      sendFrame: (frame) => {
        serverOutbound.push(frame);
      },
      supportedVersions: ["0.3"],
    });

    await client.start({
      authorization: "Bearer token",
      bufferWindowMs: 1_000,
    });
    await server.receive(outbound[0]!);
    await client.receive(serverOutbound[0]!);

    expect(client.state).toBe("closed");
    expect(client.supportedVersionsFromError).toEqual(["0.3"]);
  });

  it("responds to ping with pong and reports RTT", async () => {
    const measurements: number[] = [];
    const outbound: CulpeoFrame[] = [];
    let now = 100;
    const client = new CulpeoClientSession({
      streams: [{ content_type: "audio/opus", type: "output" }],
      sendFrame: (frame) => {
        outbound.push(frame);
      },
      onRtt: (measurement) => {
        measurements.push(measurement.rttMicros);
      },
      nowMicros: () => now,
    });

    await client.start({
      authorization: "Bearer token",
      bufferWindowMs: 1_000,
    });
    await client.receive({
      kind: "control",
      event: "culpeo.init-ack",
      headers: {
        event: "culpeo.init-ack",
        sessionId: "session-1",
        contentType: "application/json",
        bufferWindow: 1_000,
      },
      body: {
        version: "0.3",
        streams: [
          { id: "stream-1", content_type: "audio/opus", type: "output" },
        ],
      },
    });

    await client.sendPing();
    now = 175;
    await client.receive({
      kind: "control",
      event: "culpeo.pong",
      headers: { event: "culpeo.pong", contentType: "application/json" },
      body: { ts: 100, server_ts: 130 },
    });

    expect(measurements).toEqual([75]);
  });

  it("handles auth refresh without leaking tokens in errors", async () => {
    const outbound: CulpeoFrame[] = [];
    const client = new CulpeoClientSession({
      streams: [{ content_type: "audio/opus", type: "output" }],
      sendFrame: (frame) => {
        outbound.push(frame);
      },
      refreshAuthToken: async () => "refreshed-token",
    });

    await client.start({
      authorization: "Bearer old-token",
      bufferWindowMs: 1_000,
    });
    await client.receive({
      kind: "control",
      event: "culpeo.init-ack",
      headers: {
        event: "culpeo.init-ack",
        sessionId: "session-1",
        contentType: "application/json",
        bufferWindow: 1_000,
      },
      body: {
        version: "0.3",
        streams: [
          { id: "stream-1", content_type: "audio/opus", type: "output" },
        ],
      },
    });

    await client.receive({
      kind: "control",
      event: "culpeo.auth-refresh",
      headers: {
        event: "culpeo.auth-refresh",
        contentType: "application/json",
      },
      body: { nonce: "nonce-123" },
    });

    expect(outbound[1]).toMatchObject({
      event: "culpeo.auth-response",
      headers: { authorization: "Bearer refreshed-token" },
      body: { nonce: "nonce-123" },
    });

    const failingOutbound: CulpeoFrame[] = [];
    const failingClient = new CulpeoClientSession({
      streams: [{ content_type: "audio/opus", type: "output" }],
      sendFrame: (frame) => {
        failingOutbound.push(frame);
      },
      refreshAuthToken: async () => {
        throw new Error("token failure: refreshed-token");
      },
    });

    await failingClient.start({
      authorization: "Bearer old-token",
      bufferWindowMs: 1_000,
    });
    await failingClient.receive({
      kind: "control",
      event: "culpeo.init-ack",
      headers: {
        event: "culpeo.init-ack",
        sessionId: "session-2",
        contentType: "application/json",
        bufferWindow: 1_000,
      },
      body: {
        version: "0.3",
        streams: [
          { id: "stream-2", content_type: "audio/opus", type: "output" },
        ],
      },
    });

    await expect(
      failingClient.receive({
        kind: "control",
        event: "culpeo.auth-refresh",
        headers: {
          event: "culpeo.auth-refresh",
          contentType: "application/json",
        },
        body: { nonce: "nonce-999" },
      }),
    ).rejects.toThrow("Credential refresh failed.");

    expect(failingOutbound.at(-1)).toMatchObject({
      event: "culpeo.close",
      headers: { code: "auth-expired" },
    });
  });

  it("validates auth-response nonce on the server", async () => {
    const outbound: CulpeoFrame[] = [];
    const server = new CulpeoServerSession({
      sendFrame: (frame) => {
        outbound.push(frame);
      },
      generateId: (() => {
        const ids = ["session-1", "stream-1", "nonce-1"];
        return () => ids.shift() ?? "fallback";
      })(),
    });

    await server.receive({
      kind: "control",
      event: "culpeo.init",
      headers: {
        event: "culpeo.init",
        authorization: "Bearer token",
        contentType: "application/json",
        bufferWindow: 1_000,
      },
      body: {
        version: "0.3",
        streams: [{ content_type: "audio/opus", type: "output" }],
      },
    });

    await server.requestAuthRefresh();
    await expect(
      server.receive({
        kind: "control",
        event: "culpeo.auth-response",
        headers: {
          event: "culpeo.auth-response",
          authorization: "Bearer next",
          contentType: "application/json",
        },
        body: { nonce: "wrong" },
      }),
    ).rejects.toThrow(/Authentication challenge failed/);
  });

  it("drops excess pings without closing the server session", async () => {
    const outbound: CulpeoFrame[] = [];
    let now = 0;
    const server = new CulpeoServerSession({
      sendFrame: (frame) => {
        outbound.push(frame);
      },
      nowMicros: () => now,
      generateId: (() => {
        const ids = ["session-1", "stream-1"];
        return () => ids.shift() ?? "fallback";
      })(),
    });

    await establishServer(server, outbound);

    for (let index = 0; index < 5; index += 1) {
      now = index * 200_000;
      await server.receive({
        kind: "control",
        event: "culpeo.ping",
        headers: { event: "culpeo.ping", contentType: "application/json" },
        body: { ts: index },
      });
    }

    now = 900_000;
    await expect(
      server.receive({
        kind: "control",
        event: "culpeo.ping",
        headers: { event: "culpeo.ping", contentType: "application/json" },
        body: { ts: 99 },
      }),
    ).resolves.toBeUndefined();

    expect(server.state).toBe("established");
    expect(
      outbound.filter(
        (frame) => frame.kind === "control" && frame.event === "culpeo.pong",
      ),
    ).toHaveLength(5);
    expect(
      outbound.some(
        (frame) => frame.kind === "control" && frame.event === "culpeo.close",
      ),
    ).toBe(false);
  });

  it("rejects media frames whose content type does not match the stream", async () => {
    const notifications: CulpeoFrame[] = [];
    const client = new CulpeoClientSession({
      streams: [{ content_type: "audio/opus", type: "output" }],
      onNotification: (notification) => {
        if (notification.type === "media") {
          notifications.push(notification.frame);
        }
      },
    });

    await client.start({
      authorization: "Bearer token",
      bufferWindowMs: 1_000,
    });
    await client.receive({
      kind: "control",
      event: "culpeo.init-ack",
      headers: {
        event: "culpeo.init-ack",
        sessionId: "session-1",
        contentType: "application/json",
      },
      body: {
        version: "0.3",
        streams: [
          { id: "stream-1", content_type: "audio/opus", type: "output" },
        ],
      },
    });

    await expect(
      client.receive({
        kind: "media",
        headers: {
          streamId: "stream-1",
          offset: 0,
          contentType: "audio/aac",
        },
        body: new Uint8Array([1, 2, 3]),
      }),
    ).rejects.toThrow(
      "Media frame content type does not match stream declaration.",
    );

    expect(notifications).toHaveLength(0);
  });

  it("rejects media frames with offset gaps", async () => {
    const client = new CulpeoClientSession({
      streams: [{ content_type: "audio/opus", type: "output" }],
    });

    await client.start({
      authorization: "Bearer token",
      bufferWindowMs: 1_000,
    });
    await client.receive({
      kind: "control",
      event: "culpeo.init-ack",
      headers: {
        event: "culpeo.init-ack",
        sessionId: "session-1",
        contentType: "application/json",
      },
      body: {
        version: "0.3",
        streams: [
          { id: "stream-1", content_type: "audio/opus", type: "output" },
        ],
      },
    });

    await client.receive({
      kind: "media",
      headers: {
        streamId: "stream-1",
        offset: 0,
        contentType: "audio/opus",
      },
      body: new Uint8Array([1, 2, 3]),
    });

    await expect(
      client.receive({
        kind: "media",
        headers: {
          streamId: "stream-1",
          offset: 2,
          contentType: "audio/opus",
        },
        body: new Uint8Array([4, 5, 6]),
      }),
    ).rejects.toThrow(/strictly contiguous/);
  });

  it("clamps the negotiated buffer window to the server maximum", async () => {
    const outbound: CulpeoFrame[] = [];
    const server = new CulpeoServerSession({
      maxBufferWindowMs: 500,
      sendFrame: (frame) => {
        outbound.push(frame);
      },
      generateId: (() => {
        const ids = ["session-1", "stream-1"];
        return () => ids.shift() ?? "fallback";
      })(),
    });

    await establishServer(server, outbound);

    expect(server.bufferWindowMs).toBe(500);
    expect(outbound[0]).toMatchObject({
      event: "culpeo.init-ack",
      headers: { bufferWindow: 500 },
    });
  });

  it("rejects expired session resumptions based on disconnect time", async () => {
    const outbound: CulpeoFrame[] = [];
    let now = 10_000_000;
    const server = new CulpeoServerSession({
      nowMicros: () => now,
      disconnectedAtMs: 1_000,
      resumeSnapshot: {
        sessionId: "session-1",
        version: "0.3",
        bufferWindowMs: 1_000,
        disconnectedAtMs: 1_000,
        streams: [
          { id: "stream-1", content_type: "audio/opus", type: "output" },
        ],
      },
      sendFrame: (frame) => {
        outbound.push(frame);
      },
    });

    await server.receive({
      kind: "control",
      event: "culpeo.init",
      headers: {
        event: "culpeo.init",
        authorization: "Bearer token",
        contentType: "application/json",
        bufferWindow: 1_000,
        sessionId: "session-1",
      },
      body: {
        version: "0.3",
        streams: [
          { id: "stream-1", content_type: "audio/opus", type: "output" },
        ],
      },
    });

    expect(server.state).toBe("closed");
    expect(outbound[0]).toMatchObject({
      event: "culpeo.init-error",
      headers: { code: "invalid-session" },
    });
  });

  it("rejects resume declarations that do not exactly match the session", async () => {
    const outbound: CulpeoFrame[] = [];
    const server = new CulpeoServerSession({
      resumeSnapshot: {
        sessionId: "session-1",
        version: "0.3",
        bufferWindowMs: 1_000,
        streams: [
          {
            id: "stream-1",
            content_type: "audio/opus",
            type: "output",
            purpose: "assistant",
            resume_offset: 1,
          },
        ],
      },
      sendFrame: (frame) => {
        outbound.push(frame);
      },
    });

    await server.receive({
      kind: "control",
      event: "culpeo.init",
      headers: {
        event: "culpeo.init",
        authorization: "Bearer token",
        contentType: "application/json",
        bufferWindow: 1_000,
        sessionId: "session-1",
      },
      body: {
        version: "0.3",
        streams: [
          {
            id: "stream-1",
            content_type: "audio/opus",
            type: "output",
            purpose: "assistant",
            resume_offset: 2,
          },
        ],
      },
    });

    expect(server.state).toBe("closed");
    expect(outbound[0]).toMatchObject({
      event: "culpeo.init-error",
      headers: { code: "invalid-streams" },
    });
  });

  it("expires auth challenges after the configured timeout", async () => {
    const outbound: CulpeoFrame[] = [];
    let now = 0;
    const server = new CulpeoServerSession({
      authChallengeTimeoutMs: 10,
      nowMicros: () => now,
      sendFrame: (frame) => {
        outbound.push(frame);
      },
      generateId: (() => {
        const ids = ["session-1", "stream-1", "nonce-1"];
        return () => ids.shift() ?? "fallback";
      })(),
    });

    await establishServer(server, outbound);
    await server.requestAuthRefresh();

    now = 11_000;
    await expect(server.checkTimeouts()).rejects.toThrow(/timed out/);

    expect(server.state).toBe("closed");
    expect(outbound.at(-1)).toMatchObject({
      event: "culpeo.close",
      headers: { code: "auth-expired" },
    });
  });

  it("allows pings within the configured rate limit window", async () => {
    const outbound: CulpeoFrame[] = [];
    let now = 0;
    const server = new CulpeoServerSession({
      sendFrame: (frame) => {
        outbound.push(frame);
      },
      nowMicros: () => now,
      generateId: (() => {
        const ids = ["session-1", "stream-1"];
        return () => ids.shift() ?? "fallback";
      })(),
    });

    await establishServer(server, outbound);

    for (const [index, timestamp] of [
      0, 200_000, 400_000, 600_000, 800_000, 1_000_000,
    ].entries()) {
      now = timestamp;
      await expect(
        server.receive({
          kind: "control",
          event: "culpeo.ping",
          headers: { event: "culpeo.ping", contentType: "application/json" },
          body: { ts: index },
        }),
      ).resolves.toBeUndefined();
    }

    expect(server.state).toBe("established");
    expect(
      outbound.filter(
        (frame) => frame.kind === "control" && frame.event === "culpeo.pong",
      ),
    ).toHaveLength(6);
    expect(
      outbound.some(
        (frame) => frame.kind === "control" && frame.event === "culpeo.close",
      ),
    ).toBe(false);
  });
});
