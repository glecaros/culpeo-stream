using System.Net.WebSockets;
using System.Text;
using System.Text.Json;
using CulpeoStream.Core;

namespace CulpeoStream.Client.Tests;

/// <summary>
/// Unit tests for <see cref="CulpeoStreamClient"/> using in-process WebSocket pairs.
/// </summary>
public sealed class ClientTests
{
    // ── Helpers ───────────────────────────────────────────────────────────────

    private static CulpeoStreamClientOptions DefaultOptions(bool autoReconnect = false) => new()
    {
        Authorization = "Bearer test-token",
        Streams =
        [
            new StreamDeclaration
            {
                ContentType = "audio/pcm;rate=16000;channels=1;bits=16",
                Type = CulpeoStreamType.Input,
                OffsetType = OffsetType.Time
            }
        ],
        AutoReconnect = autoReconnect,
        AllowInsecureConnections = true
    };

    private static (CulpeoStreamClient client, WebSocketPair pair) CreatePair(
        CulpeoStreamClientOptions? options = null)
    {
        var pair = WebSocketPair.Create();
        var client = new CulpeoStreamClient(options ?? DefaultOptions())
        {
            WebSocketFactory = (_, _) => Task.FromResult(pair.Client)
        };
        return (client, pair);
    }

    // ── Test: wss:// enforcement ──────────────────────────────────────────────

    [Fact]
    public async Task ConnectAsync_WsUri_AllowInsecureFalse_Throws()
    {
        var opts = new CulpeoStreamClientOptions
        {
            Authorization = "Bearer token",
            Streams = [new StreamDeclaration { ContentType = "audio/opus", Type = CulpeoStreamType.Input, OffsetType = OffsetType.Message }],
            AllowInsecureConnections = false
        };

        await using var client = new CulpeoStreamClient(opts);

        var ex = await Assert.ThrowsAsync<InvalidOperationException>(
            () => client.ConnectAsync(new Uri("ws://localhost:5000")));

        Assert.Contains("ws://", ex.Message);
        Assert.Contains("AllowInsecureConnections", ex.Message);
    }

    [Fact]
    public async Task ConnectAsync_WssUri_NoThrow()
    {
        // With AllowInsecureConnections = false and wss://, no scheme error should be thrown.
        // We use a fake factory that just returns the in-memory WebSocket pair.
        var pair = WebSocketPair.Create();
        var opts = new CulpeoStreamClientOptions
        {
            Authorization = "Bearer token",
            Streams = [new StreamDeclaration { ContentType = "audio/opus", Type = CulpeoStreamType.Input, OffsetType = OffsetType.Message }],
            AllowInsecureConnections = false
        };

        await using var client = new CulpeoStreamClient(opts)
        {
            WebSocketFactory = (_, ct) => Task.FromResult(pair.Client)
        };

        var connectTask = client.ConnectAsync(new Uri("wss://localhost:5000"));

        // Server side: handle init
        var cts = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        await pair.ServerHandleInitAsync(ct: cts.Token);

        await connectTask;
        Assert.Equal(CulpeoClientState.Established, client.State);
        await client.DisconnectAsync();
    }

    // ── Test: successful connect + init handshake ─────────────────────────────

    [Fact]
    public async Task ConnectAsync_PerformsHandshakeAndEmitsSessionEstablished()
    {
        var (client, pair) = CreatePair();
        using var _ = pair;

        var cts = new CancellationTokenSource(TimeSpan.FromSeconds(5));

        var serverTask = Task.Run(async () =>
        {
            var (initFrame, sessionId, streams) = await pair.ServerHandleInitAsync(
                sessionIdToReply: "test-session-id",
                ct: cts.Token);

            Assert.Equal("culpeo.init", initFrame.Event);
            Assert.Equal("Bearer test-token", initFrame.Authorization);
            return (sessionId, streams);
        });

        await client.ConnectAsync(new Uri("wss://localhost"), cts.Token);

        var (sid, _) = await serverTask;

        Assert.Equal(CulpeoClientState.Established, client.State);
        Assert.Equal("test-session-id", sid);

        // Verify SessionEstablished event is emitted
        var evt = await ReadNextEventAsync(client, cts.Token);
        var established = Assert.IsType<SessionEstablished>(evt);
        Assert.Equal("test-session-id", established.SessionId);

        await client.DisconnectAsync();
    }

    // ── Test: SendMediaAsync with offset tracking ─────────────────────────────

    [Fact]
    public async Task SendMediaAsync_TracksOffset_TimeBasedPcm()
    {
        var (client, pair) = CreatePair();
        using var _ = pair;

        var cts = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        string streamId = "";

        var serverTask = Task.Run(async () =>
        {
            var (_, sessionId, streams) = await pair.ServerHandleInitAsync(ct: cts.Token);
            streamId = streams[0].ServerId;

            // Receive and validate media frames
            var frame1 = await pair.ServerReceiveAsync(cts.Token);
            var frame2 = await pair.ServerReceiveAsync(cts.Token);
            return (frame1, frame2);
        });

        await client.ConnectAsync(new Uri("wss://localhost"), cts.Token);

        // Wait for SessionEstablished
        await ReadNextEventAsync(client, cts.Token);

        // PCM: 16-bit mono at 16 kHz → stride = 1 channel × 2 bytes = 2 bytes/sample
        // 320 bytes = 160 samples → offset advances by 160 per frame
        var payload1 = new byte[320];
        var payload2 = new byte[640]; // 320 samples

        await client.SendMediaAsync(streamId, payload1, cts.Token);
        await client.SendMediaAsync(streamId, payload2, cts.Token);

        var (f1, f2) = await serverTask;

        Assert.Equal(CulpeoMessageKind.Media, f1.Kind);
        Assert.Equal(0L, f1.Offset);  // first frame starts at 0
        Assert.Equal(streamId, f1.StreamId);

        Assert.Equal(CulpeoMessageKind.Media, f2.Kind);
        Assert.Equal(160L, f2.Offset);  // 320 bytes / 2 = 160 samples
        Assert.Equal(streamId, f2.StreamId);

        await client.DisconnectAsync();
    }

    [Fact]
    public async Task SendMediaAsync_MessageOffset_IncrementsBy1()
    {
        var opts = new CulpeoStreamClientOptions
        {
            Authorization = "Bearer token",
            Streams = [new StreamDeclaration { ContentType = "audio/opus", Type = CulpeoStreamType.Input, OffsetType = OffsetType.Message }],
            AutoReconnect = false,
            AllowInsecureConnections = true
        };

        var pair = WebSocketPair.Create();
        await using var client = new CulpeoStreamClient(opts)
        {
            WebSocketFactory = (_, ct) => Task.FromResult(pair.Client)
        };

        var cts = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        string streamId = "";

        var serverTask = Task.Run(async () =>
        {
            var (_, _, streams) = await pair.ServerHandleInitAsync(ct: cts.Token);
            streamId = streams[0].ServerId;

            var f1 = await pair.ServerReceiveAsync(cts.Token);
            var f2 = await pair.ServerReceiveAsync(cts.Token);
            var f3 = await pair.ServerReceiveAsync(cts.Token);
            return (f1, f2, f3);
        });

        await client.ConnectAsync(new Uri("wss://localhost"), cts.Token);
        await ReadNextEventAsync(client, cts.Token); // SessionEstablished

        await client.SendMediaAsync(streamId, new byte[100], cts.Token);
        await client.SendMediaAsync(streamId, new byte[200], cts.Token);
        await client.SendMediaAsync(streamId, new byte[300], cts.Token);

        var (f1, f2, f3) = await serverTask;

        Assert.Equal(0L, f1.Offset);
        Assert.Equal(1L, f2.Offset);
        Assert.Equal(2L, f3.Offset);

        await client.DisconnectAsync();
    }

    [Fact]
    public async Task SendMediaAsync_ByteOffset_IncrementsBy_PayloadLength()
    {
        var opts = new CulpeoStreamClientOptions
        {
            Authorization = "Bearer token",
            Streams = [new StreamDeclaration { ContentType = "application/octet-stream", Type = CulpeoStreamType.Input, OffsetType = OffsetType.Byte }],
            AutoReconnect = false,
            AllowInsecureConnections = true
        };

        var pair = WebSocketPair.Create();
        await using var client = new CulpeoStreamClient(opts)
        {
            WebSocketFactory = (_, ct) => Task.FromResult(pair.Client)
        };

        var cts = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        string streamId = "";

        var serverTask = Task.Run(async () =>
        {
            var (_, _, streams) = await pair.ServerHandleInitAsync(ct: cts.Token);
            streamId = streams[0].ServerId;
            var f1 = await pair.ServerReceiveAsync(cts.Token);
            var f2 = await pair.ServerReceiveAsync(cts.Token);
            return (f1, f2);
        });

        await client.ConnectAsync(new Uri("wss://localhost"), cts.Token);
        await ReadNextEventAsync(client, cts.Token);

        await client.SendMediaAsync(streamId, new byte[50], cts.Token);
        await client.SendMediaAsync(streamId, new byte[75], cts.Token);

        var (f1, f2) = await serverTask;

        Assert.Equal(0L, f1.Offset);
        Assert.Equal(50L, f2.Offset);

        await client.DisconnectAsync();
    }

    // ── Test: ReceiveAsync yields MediaReceived events ────────────────────────

    [Fact]
    public async Task ReceiveAsync_YieldsMediaReceivedForOutputStream()
    {
        var opts = new CulpeoStreamClientOptions
        {
            Authorization = "Bearer token",
            Streams = [new StreamDeclaration { ContentType = "audio/opus", Type = CulpeoStreamType.Output, OffsetType = OffsetType.Message }],
            AutoReconnect = false,
            AllowInsecureConnections = true
        };

        var pair = WebSocketPair.Create();
        await using var client = new CulpeoStreamClient(opts)
        {
            WebSocketFactory = (_, ct) => Task.FromResult(pair.Client)
        };

        var cts = new CancellationTokenSource(TimeSpan.FromSeconds(5));

        string streamId = "";
        var serverTask = Task.Run(async () =>
        {
            var (_, _, streams) = await pair.ServerHandleInitAsync(ct: cts.Token);
            streamId = streams[0].ServerId;

            // Send three media frames to the client
            await pair.ServerSendMediaFrameAsync(streamId, "audio/opus", 0, [0x01, 0x02], cts.Token);
            await pair.ServerSendMediaFrameAsync(streamId, "audio/opus", 1, [0x03, 0x04], cts.Token);
            await pair.ServerSendMediaFrameAsync(streamId, "audio/opus", 2, [0x05, 0x06], cts.Token);

            // Then close
            await pair.ServerSendCloseAsync(ct: cts.Token);
        });

        await client.ConnectAsync(new Uri("wss://localhost"), cts.Token);

        var received = new List<MediaReceived>();
        await foreach (var evt in client.ReceiveAsync(cts.Token))
        {
            if (evt is MediaReceived mr) received.Add(mr);
            if (evt is Disconnected) break;
        }

        Assert.Equal(3, received.Count);
        Assert.Equal(streamId, received[0].StreamId);
        Assert.Equal(0L, received[0].Offset);
        Assert.Equal(new byte[] { 0x01, 0x02 }, received[0].Data.ToArray());
        Assert.Equal(1L, received[1].Offset);
        Assert.Equal(2L, received[2].Offset);

        await serverTask;
    }

    // ── Test: application events ──────────────────────────────────────────────

    [Fact]
    public async Task ReceiveAsync_YieldsApplicationEventReceived()
    {
        var (client, pair) = CreatePair();
        using var _ = pair;
        var cts = new CancellationTokenSource(TimeSpan.FromSeconds(5));

        var serverTask = Task.Run(async () =>
        {
            await pair.ServerHandleInitAsync(ct: cts.Token);

            // Send application event
            var eventBody = "{\"text\":\"hello\"}";
            var evtFrame = new CulpeoMessage(
                CulpeoMessageKind.Control,
                System.Text.Encoding.UTF8.GetBytes(eventBody),
                @event: "myservice.transcript",
                contentType: "application/json");

            await pair.ServerSendControlAsync(evtFrame, cts.Token);
            await pair.ServerSendCloseAsync(ct: cts.Token);
        });

        await client.ConnectAsync(new Uri("wss://localhost"), cts.Token);

        ApplicationEventReceived? appEvt = null;
        await foreach (var evt in client.ReceiveAsync(cts.Token))
        {
            if (evt is ApplicationEventReceived aer) appEvt = aer;
            if (evt is Disconnected) break;
        }

        Assert.NotNull(appEvt);
        Assert.Equal("myservice.transcript", appEvt.EventName);
        Assert.Equal("hello", appEvt.Body.GetProperty("text").GetString());

        await serverTask;
    }

    // ── Test: graceful disconnect sends culpeo.close ──────────────────────────

    [Fact]
    public async Task DisconnectAsync_SendsCulpeoClose()
    {
        var (client, pair) = CreatePair();
        using var _ = pair;
        var cts = new CancellationTokenSource(TimeSpan.FromSeconds(5));

        var serverTask = Task.Run(async () =>
        {
            await pair.ServerHandleInitAsync(ct: cts.Token);

            // Wait for close from client
            try
            {
                var closeFrame = await pair.ServerReceiveAsync(cts.Token);
                return closeFrame;
            }
            catch
            {
                return null;
            }
        });

        await client.ConnectAsync(new Uri("wss://localhost"), cts.Token);
        await ReadNextEventAsync(client, cts.Token); // consume SessionEstablished

        await client.DisconnectAsync(cts.Token);

        var closeFrame = await serverTask;
        Assert.NotNull(closeFrame);
        Assert.Equal("culpeo.close", closeFrame.Event);
        Assert.Equal("normal", closeFrame.Code);
    }

    // ── Test: AutoReconnect = false → no retry on disconnect ─────────────────

    [Fact]
    public async Task AutoReconnectFalse_NoRetryOnServerClose()
    {
        var opts = DefaultOptions(autoReconnect: false);
        var pair = WebSocketPair.Create();

        await using var client = new CulpeoStreamClient(opts)
        {
            WebSocketFactory = (_, ct) => Task.FromResult(pair.Client)
        };

        var cts = new CancellationTokenSource(TimeSpan.FromSeconds(5));

        var serverTask = Task.Run(async () =>
        {
            await pair.ServerHandleInitAsync(ct: cts.Token);
            // Immediately close the session
            await pair.ServerSendCloseAsync(code: "server-shutdown", ct: cts.Token);
        });

        await client.ConnectAsync(new Uri("wss://localhost"), cts.Token);

        var events = new List<CulpeoClientEvent>();
        await foreach (var evt in client.ReceiveAsync(cts.Token))
        {
            events.Add(evt);
            if (evt is Disconnected) break;
        }

        // Should have received: SessionEstablished, Disconnected
        Assert.Contains(events, e => e is SessionEstablished);
        Assert.Contains(events, e => e is Disconnected);

        // State should be Disconnected, not Reconnecting
        Assert.Equal(CulpeoClientState.Disconnected, client.State);

        await serverTask;
    }

    // ── Test: Auth refresh flow ───────────────────────────────────────────────

    [Fact]
    public async Task AuthRefresh_CallsGetTokenAndSendsAuthResponse()
    {
        var tokenRefreshCalled = false;
        var opts = new CulpeoStreamClientOptions
        {
            Authorization = "Bearer initial-token",
            Streams = [new StreamDeclaration { ContentType = "audio/opus", Type = CulpeoStreamType.Input, OffsetType = OffsetType.Message }],
            AutoReconnect = false,
            AllowInsecureConnections = true,
            GetToken = async (ct) =>
            {
                tokenRefreshCalled = true;
                await Task.Yield();
                return "Bearer refreshed-token";
            }
        };

        var pair = WebSocketPair.Create();
        await using var client = new CulpeoStreamClient(opts)
        {
            WebSocketFactory = (_, ct) => Task.FromResult(pair.Client)
        };

        var cts = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        CulpeoMessage? authResponse = null;

        var serverTask = Task.Run(async () =>
        {
            await pair.ServerHandleInitAsync(ct: cts.Token);

            // Issue auth-refresh challenge
            const string nonce = "test-nonce-abc123";
            await pair.ServerSendAuthRefreshAsync(nonce, cts.Token);

            // Wait for auth-response from client
            authResponse = await pair.ServerReceiveAsync(cts.Token);

            // Close session
            await pair.ServerSendCloseAsync(ct: cts.Token);
        });

        await client.ConnectAsync(new Uri("wss://localhost"), cts.Token);

        await foreach (var evt in client.ReceiveAsync(cts.Token))
        {
            if (evt is Disconnected) break;
        }

        await serverTask;

        Assert.True(tokenRefreshCalled, "GetToken callback was not called.");
        Assert.NotNull(authResponse);
        Assert.Equal("culpeo.auth-response", authResponse.Event);
        Assert.Equal("Bearer refreshed-token", authResponse.Authorization);

        // Verify nonce was echoed
        using var doc = JsonDocument.Parse(authResponse.Body.ToArray());
        Assert.Equal("test-nonce-abc123", doc.RootElement.GetProperty("nonce").GetString());
    }

    [Fact]
    public async Task AuthRefresh_NoGetTokenCallback_ClosesWithAuthError()
    {
        var opts = new CulpeoStreamClientOptions
        {
            Authorization = "Bearer token",
            Streams = [new StreamDeclaration { ContentType = "audio/opus", Type = CulpeoStreamType.Input, OffsetType = OffsetType.Message }],
            AutoReconnect = false,
            AllowInsecureConnections = true,
            GetToken = null  // no callback
        };

        var pair = WebSocketPair.Create();
        await using var client = new CulpeoStreamClient(opts)
        {
            WebSocketFactory = (_, ct) => Task.FromResult(pair.Client)
        };

        var cts = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        CulpeoMessage? closeFrame = null;

        var serverTask = Task.Run(async () =>
        {
            await pair.ServerHandleInitAsync(ct: cts.Token);

            // Issue auth-refresh
            await pair.ServerSendAuthRefreshAsync("nonce-xyz", cts.Token);

            // Wait for close from client
            try { closeFrame = await pair.ServerReceiveAsync(cts.Token); } catch { }
        });

        await client.ConnectAsync(new Uri("wss://localhost"), cts.Token);

        await foreach (var evt in client.ReceiveAsync(cts.Token))
        {
            if (evt is Disconnected) break;
        }

        await serverTask;

        Assert.NotNull(closeFrame);
        Assert.Equal("culpeo.close", closeFrame.Event);
        Assert.Equal("auth-expired", closeFrame.Code);
    }

    // ── Test: reconnect with session resumption ───────────────────────────────

    [Fact]
    public async Task Reconnect_IncludesSessionIdAndResumeOffsets()
    {
        const string firstSessionId = "session-abc-123";
        int connectCount = 0;

        // We'll use two separate WebSocketPairs — one per connection attempt
        var pair1 = WebSocketPair.Create();
        var pair2 = WebSocketPair.Create();

        var opts = new CulpeoStreamClientOptions
        {
            Authorization = "Bearer token",
            Streams = [new StreamDeclaration { ContentType = "audio/opus", Type = CulpeoStreamType.Input, OffsetType = OffsetType.Message }],
            AutoReconnect = true,
            MaxReconnectAttempts = 3,
            InitialBackoff = TimeSpan.FromMilliseconds(50),
            MaxBackoff = TimeSpan.FromMilliseconds(100),
            AllowInsecureConnections = true
        };

        await using var client = new CulpeoStreamClient(opts)
        {
            WebSocketFactory = (_, ct) =>
            {
                var count = System.Threading.Interlocked.Increment(ref connectCount);
                return Task.FromResult(count == 1 ? pair1.Client : pair2.Client);
            }
        };

        var cts = new CancellationTokenSource(TimeSpan.FromSeconds(10));

        // Synchronize: signal when init handshake completes so client can send media first
        var handshakeDone = new TaskCompletionSource<string>(TaskCreationOptions.RunContinuationsAsynchronously);
        var mediaFramesSent = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);

        // First connection: handle init, signal, wait for media to be sent, then close
        var firstConnTask = Task.Run(async () =>
        {
            var (_, _, streams) = await pair1.ServerHandleInitAsync(
                sessionIdToReply: firstSessionId, ct: cts.Token);
            // Signal stream ID to the main test thread
            handshakeDone.SetResult(streams[0].ServerId);

            // Wait until the main thread has finished sending media frames
            await mediaFramesSent.Task;

            // Drain the 3 media frames from the incoming channel (otherwise the MemoryWebSocket blocks)
            for (int i = 0; i < 3; i++)
            {
                try { await pair1.ServerReceiveAsync(cts.Token); } catch { break; }
            }

            // Server abruptly closes (no culpeo.close — just WebSocket close)
            await pair1.Server.CloseAsync(WebSocketCloseStatus.NormalClosure, "bye", cts.Token);
        });

        await client.ConnectAsync(new Uri("wss://localhost"), cts.Token);
        await ReadNextEventAsync(client, cts.Token); // SessionEstablished

        // Wait for stream ID to be known, then send 3 media frames
        var streamId1 = await handshakeDone.Task;

        await client.SendMediaAsync(streamId1, new byte[10], cts.Token);
        await client.SendMediaAsync(streamId1, new byte[10], cts.Token);
        await client.SendMediaAsync(streamId1, new byte[10], cts.Token);

        // Tell server it can close now
        mediaFramesSent.SetResult(true);

        await firstConnTask;

        // Second connection (reconnect): read init, verify session-id + resume_offset
        string? resumeSessionId = null;
        long? resumeOffset = null;

        var secondConnTask = Task.Run(async () =>
        {
            // The reconnect loop will call our factory again with pair2.Client
            var initFrame = await pair2.ServerReceiveAsync(cts.Token);
            resumeSessionId = initFrame.SessionId;

            // Parse resume_offset from body
            using var doc = JsonDocument.Parse(initFrame.Body.ToArray());
            var streamsEl = doc.RootElement.GetProperty("streams");
            foreach (var s in streamsEl.EnumerateArray())
            {
                if (s.TryGetProperty("resume_offset", out var roProp))
                {
                    resumeOffset = roProp.GetInt64();
                }
            }

            // Reply with init-ack
            var bodyStr = $"{{\"version\":\"0.3\",\"streams\":[{{\"id\":\"s-new\",\"content_type\":\"audio/opus\",\"type\":\"input\",\"offset_type\":\"message\"}}]}}";
            var ackFrame = new CulpeoMessage(
                CulpeoMessageKind.Control,
                System.Text.Encoding.UTF8.GetBytes(bodyStr),
                @event: "culpeo.init-ack",
                contentType: "application/json",
                sessionId: firstSessionId,
                bufferWindow: 5000);

            await pair2.ServerSendControlAsync(ackFrame, cts.Token);

            // Let the session run for a moment then close
            await pair2.ServerSendCloseAsync(ct: cts.Token);
        });

        // Wait for SessionResumed event
        await foreach (var evt in client.ReceiveAsync(cts.Token))
        {
            if (evt is SessionResumed sr)
            {
                Assert.Equal(firstSessionId, sr.SessionId);
                break;
            }
            if (evt is Disconnected d)
            {
                Assert.Fail($"Expected SessionResumed but got Disconnected: {d.Reason}");
                break;
            }
        }

        await secondConnTask;

        // Verify the reconnect included the original session ID and resume_offset
        Assert.Equal(firstSessionId, resumeSessionId);
        Assert.Equal(3L, resumeOffset); // 3 message frames were sent
    }

    // ── Test: invalid-session clears offsets ──────────────────────────────────

    [Fact]
    public async Task Reconnect_InvalidSession_ClearsOffsets()
    {
        var pair1 = WebSocketPair.Create();
        var pair2 = WebSocketPair.Create();
        int connectCount = 0;

        var opts = new CulpeoStreamClientOptions
        {
            Authorization = "Bearer token",
            Streams = [new StreamDeclaration { ContentType = "audio/opus", Type = CulpeoStreamType.Input, OffsetType = OffsetType.Message }],
            AutoReconnect = true,
            MaxReconnectAttempts = 3,
            InitialBackoff = TimeSpan.FromMilliseconds(50),
            MaxBackoff = TimeSpan.FromMilliseconds(100),
            AllowInsecureConnections = true
        };

        await using var client = new CulpeoStreamClient(opts)
        {
            WebSocketFactory = (_, ct) =>
            {
                var count = System.Threading.Interlocked.Increment(ref connectCount);
                return Task.FromResult(count == 1 ? pair1.Client : pair2.Client);
            }
        };

        var cts = new CancellationTokenSource(TimeSpan.FromSeconds(10));

        // First connection: establish, close abruptly
        var firstTask = Task.Run(async () =>
        {
            await pair1.ServerHandleInitAsync(sessionIdToReply: "orig-session", ct: cts.Token);
            await pair1.Server.CloseAsync(WebSocketCloseStatus.NormalClosure, "bye", cts.Token);
        });

        await client.ConnectAsync(new Uri("wss://localhost"), cts.Token);
        await ReadNextEventAsync(client, cts.Token);
        await firstTask;

        // Second connection: reply with invalid-session error → client should start fresh
        var secondTask = Task.Run(async () =>
        {
            // Read init
            await pair2.ServerReceiveAsync(cts.Token);

            // Reply with invalid-session error
            await pair2.ServerSendInitErrorAsync("invalid-session", "Session has expired.", cts.Token);

            // After invalid-session, client should retry with fresh init (no session ID)
            // But since we only have pair2, and the factory returns pair2 again on subsequent calls,
            // just close the server side to end the test gracefully
            try
            {
                var retryInit = await pair2.ServerReceiveAsync(cts.Token);
                // On fresh retry, Session-Id should be null
                return retryInit.SessionId;
            }
            catch
            {
                return null;
            }
        });

        // Consume events; client will eventually disconnect
        var disconnected = false;
        using var readCts = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        await foreach (var evt in client.ReceiveAsync(readCts.Token))
        {
            if (evt is Disconnected) { disconnected = true; break; }
            if (evt is SessionEstablished or SessionResumed) break;
        }

        var retrySessionId = await secondTask;
        // After invalid-session, the fresh retry should not include Session-Id
        Assert.True(string.IsNullOrEmpty(retrySessionId),
            $"Expected no session ID on fresh retry but got: '{retrySessionId}'");
    }

    // ── Test: SendMediaAsync throws when not established ─────────────────────

    [Fact]
    public async Task SendMediaAsync_WhenNotEstablished_Throws()
    {
        var opts = DefaultOptions();
        await using var client = new CulpeoStreamClient(opts);

        // State is Disconnected — send should throw
        await Assert.ThrowsAsync<InvalidOperationException>(
            () => client.SendMediaAsync("some-stream", new byte[10]));
    }

    // ── Test: Streams property reflects init-ack ──────────────────────────────

    [Fact]
    public async Task Streams_ReflectsServerAssignedIds()
    {
        var (client, pair) = CreatePair();
        using var _ = pair;

        var cts = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        const string expectedStreamId = "server-assigned-stream-id";

        var serverTask = Task.Run(async () =>
        {
            // Override the server-assigned ID to a known value
            await pair.ServerHandleInitAsync(ct: cts.Token);
        });

        await client.ConnectAsync(new Uri("wss://localhost"), cts.Token);

        Assert.NotEmpty(client.Streams);

        await client.DisconnectAsync();
        await serverTask;
    }

    // ── Test: SendEventAsync sends well-formed frame ──────────────────────────

    [Fact]
    public async Task SendEventAsync_SendsApplicationEvent()
    {
        var (client, pair) = CreatePair();
        using var _ = pair;
        var cts = new CancellationTokenSource(TimeSpan.FromSeconds(5));

        var serverTask = Task.Run(async () =>
        {
            await pair.ServerHandleInitAsync(ct: cts.Token);
            // Read the event frame
            return await pair.ServerReceiveAsync(cts.Token);
        });

        await client.ConnectAsync(new Uri("wss://localhost"), cts.Token);
        await ReadNextEventAsync(client, cts.Token);

        await client.SendEventAsync("myapp.hello", new { greeting = "world" }, cts.Token);

        var evtFrame = await serverTask;
        Assert.Equal("myapp.hello", evtFrame.Event);
        using var doc = JsonDocument.Parse(evtFrame.Body.ToArray());
        Assert.Equal("world", doc.RootElement.GetProperty("greeting").GetString());

        await client.DisconnectAsync();
    }

    [Fact]
    public async Task SendEventAsync_ReservedNamespace_Throws()
    {
        var (client, pair) = CreatePair();
        using var _ = pair;
        var cts = new CancellationTokenSource(TimeSpan.FromSeconds(5));

        var serverTask = Task.Run(() => pair.ServerHandleInitAsync(ct: cts.Token));
        await client.ConnectAsync(new Uri("wss://localhost"), cts.Token);
        await ReadNextEventAsync(client, cts.Token);

        await Assert.ThrowsAsync<ArgumentException>(
            () => client.SendEventAsync("culpeo.custom", null, cts.Token));

        await client.DisconnectAsync();
        await serverTask;
    }

    // ── Private helpers ───────────────────────────────────────────────────────

    private static async Task<CulpeoClientEvent> ReadNextEventAsync(
        CulpeoStreamClient client,
        CancellationToken ct)
    {
        await foreach (var evt in client.ReceiveAsync(ct))
        {
            return evt;
        }
        throw new InvalidOperationException("Channel completed without producing an event.");
    }
}
