using System.Net.WebSockets;
using System.Text;
using System.Text.Json;
using CulpeoStream.Core;
using CulpeoCore = CulpeoStream.Core;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.TestHost;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;

namespace CulpeoStream.AspNetCore.Tests;

/// <summary>
/// End-to-end integration tests using <see cref="TestServer"/>.
/// Tests cover the full session lifecycle, handler callback ordering,
/// concurrent sessions, and edge-case protocol behaviors.
/// </summary>
public sealed class IntegrationTests
{
    // ── Test server factory ────────────────────────────────────────────────────

    private static TestServer CreateServer(
        ICulpeoStreamHandler handler,
        Action<CulpeoStreamOptions>? configureOptions = null)
    {
        var builder = new WebHostBuilder()
            .UseEnvironment(Environments.Development)
            .ConfigureServices(services =>
            {
                services.AddRouting();
                services.AddCulpeoStream(configureOptions);
            })
            .Configure(app =>
            {
                app.UseRouting();
                app.UseEndpoints(ep => ep.MapCulpeoStream("/culpeo", handler));
            });

        return new TestServer(builder);
    }

    private static async Task<WebSocket> ConnectAsync(TestServer server, CancellationToken ct = default)
    {
        var wsClient = server.CreateWebSocketClient();
        wsClient.SubProtocols.Add("culpeostream");
        return await wsClient.ConnectAsync(new Uri("ws://localhost/culpeo"), ct);
    }

    // ── Session establishment ──────────────────────────────────────────────────

    [Fact]
    public async Task SuccessfulInit_Returns_InitAck_With_SessionId()
    {
        using var server = CreateServer(new NoopHandler());
        using var ws = await ConnectAsync(server);

        var response = await MessageHelper.InitSessionAsync(ws);

        Assert.Equal("culpeo.init-ack", response.Event);
        Assert.NotNull(response.SessionId);
        Assert.NotEmpty(response.SessionId!);

        using var body = MessageHelper.ParseBody(response);
        Assert.Equal("0.3", body.RootElement.GetProperty("version").GetString());
        var streams = body.RootElement.GetProperty("streams");
        Assert.Equal(1, streams.GetArrayLength());
    }

    [Fact]
    public async Task Init_WithUnsupportedVersion_Returns_InitError_And_Closes()
    {
        using var server = CreateServer(new NoopHandler());
        using var ws = await ConnectAsync(server);

        var bodyBytes = Encoding.UTF8.GetBytes(
            """{"version":"9.9","streams":[{"content_type":"audio/opus","type":"input","offset_type":"message"}]}""");

        var initFrame = new CulpeoMessage(
            CulpeoMessageKind.Control,
            bodyBytes,
            @event: "culpeo.init",
            authorization: "Bearer tok",
            contentType: "application/json");

        await MessageHelper.SendControlFrameAsync(ws, initFrame);
        var response = await MessageHelper.ReceiveFrameAsync(ws);

        Assert.Equal("culpeo.init-error", response.Event);
        Assert.Equal("unsupported-version", response.Code);

        using var body = MessageHelper.ParseBody(response);
        Assert.True(body.RootElement.TryGetProperty("supported_versions", out var versions));
        Assert.Contains("0.3", versions.EnumerateArray().Select(v => v.GetString()));
    }

    [Fact]
    public async Task Init_WithoutAuthorization_Returns_InitError_Unauthorized()
    {
        using var server = CreateServer(new NoopHandler());
        using var ws = await ConnectAsync(server);

        var bodyBytes = Encoding.UTF8.GetBytes(
            """{"version":"0.3","streams":[{"content_type":"audio/opus","type":"input","offset_type":"message"}]}""");

        var initFrame = new CulpeoMessage(
            CulpeoMessageKind.Control,
            bodyBytes,
            @event: "culpeo.init",
            // no Authorization header
            contentType: "application/json");

        await MessageHelper.SendControlFrameAsync(ws, initFrame);
        var response = await MessageHelper.ReceiveFrameAsync(ws);

        Assert.Equal("culpeo.init-error", response.Event);
        Assert.Equal("unauthorized", response.Code);
    }

    [Fact]
    public async Task NonInitFirstFrame_Returns_CloseFrame_With_ProtocolError()
    {
        using var server = CreateServer(new NoopHandler());
        using var ws = await ConnectAsync(server);

        // Send a ping before init
        var pingFrame = new CulpeoMessage(
            CulpeoMessageKind.Control,
            Encoding.UTF8.GetBytes("""{"ts":1234567890}"""),
            @event: "culpeo.ping",
            contentType: "application/json");

        await MessageHelper.SendControlFrameAsync(ws, pingFrame);
        var response = await MessageHelper.ReceiveFrameAsync(ws);

        Assert.Equal("culpeo.close", response.Event);
        Assert.Equal("protocol-error", response.Code);
    }

    // ── Handler lifecycle callbacks ────────────────────────────────────────────

    [Fact]
    public async Task OnConnectedAsync_Called_After_InitAck()
    {
        var tcs = new TaskCompletionSource<string>(TaskCreationOptions.RunContinuationsAsynchronously);
        var handler = new CapturingHandler(onConnected: session => tcs.SetResult(session.SessionId));

        using var server = CreateServer(handler);
        using var ws = await ConnectAsync(server);

        var ack = await MessageHelper.InitSessionAsync(ws);
        Assert.Equal("culpeo.init-ack", ack.Event);

        var sessionId = await tcs.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(ack.SessionId, sessionId);
    }

    [Fact]
    public async Task OnMediaFrameAsync_Called_For_Incoming_Media()
    {
        var mediaReceived = new TaskCompletionSource<CulpeoMediaFrameContext>(
            TaskCreationOptions.RunContinuationsAsynchronously);

        var handler = new CapturingHandler(onMedia: (_, ctx) => mediaReceived.TrySetResult(ctx));
        using var server = CreateServer(handler);
        using var ws = await ConnectAsync(server);

        var ack = await MessageHelper.InitSessionAsync(ws);
        var streamId = ExtractFirstStreamId(ack);

        // Send a binary (media) frame
        var mediaFrame = new CulpeoMessage(
            CulpeoMessageKind.Media,
            new byte[] { 0x01, 0x02, 0x03, 0x04 },
            contentType: "audio/pcm;rate=16000;channels=1;bits=16",
            streamId: streamId,
            offset: 0,
            timestamp: 0);

        var serializer = new CulpeoMessageSerializer();
        var bytes = await serializer.SerializeAsync(mediaFrame);
        await ws.SendAsync(bytes.AsMemory(), WebSocketMessageType.Binary, endOfMessage: true, default);

        var received = await mediaReceived.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(streamId, received.StreamId);
        Assert.Equal(0, received.Offset);
        Assert.Equal(4, received.Payload.Length);
    }

    [Fact]
    public async Task OnEventAsync_Called_For_Application_Events()
    {
        var eventReceived = new TaskCompletionSource<CulpeoEventContext>(
            TaskCreationOptions.RunContinuationsAsynchronously);

        var handler = new CapturingHandler(onEvent: (_, ctx) => eventReceived.TrySetResult(ctx));
        using var server = CreateServer(handler);
        using var ws = await ConnectAsync(server);

        await MessageHelper.InitSessionAsync(ws);

        // Send a custom application event
        var eventFrame = new CulpeoMessage(
            CulpeoMessageKind.Control,
            Encoding.UTF8.GetBytes("""{"text":"hello"}"""),
            @event: "myapp.transcript",
            contentType: "application/json");

        await MessageHelper.SendControlFrameAsync(ws, eventFrame);

        var received = await eventReceived.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal("myapp.transcript", received.EventName);
        Assert.Contains("hello", received.JsonBody);
    }

    [Fact]
    public async Task OnDisconnectedAsync_Called_When_Client_Closes()
    {
        var disconnectTcs = new TaskCompletionSource<string?>(
            TaskCreationOptions.RunContinuationsAsynchronously);

        var handler = new CapturingHandler(
            onDisconnected: (_, code) => disconnectTcs.TrySetResult(code));

        using var server = CreateServer(handler);
        using var ws = await ConnectAsync(server);

        await MessageHelper.InitSessionAsync(ws);

        // Send culpeo.close
        var closeFrame = new CulpeoMessage(
            CulpeoMessageKind.Control,
            "{}"u8.ToArray(),
            @event: "culpeo.close",
            contentType: "application/json",
            code: "normal",
            reason: "test done");

        await MessageHelper.SendControlFrameAsync(ws, closeFrame);

        var closeCode = await disconnectTcs.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.NotNull(closeCode);
    }

    // ── Ping / pong ────────────────────────────────────────────────────────────

    [Fact]
    public async Task Ping_Returns_Pong_With_Echoed_Ts()
    {
        using var server = CreateServer(new NoopHandler());
        using var ws = await ConnectAsync(server);

        await MessageHelper.InitSessionAsync(ws);

        var ts = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() * 1000L;
        var pingFrame = new CulpeoMessage(
            CulpeoMessageKind.Control,
            Encoding.UTF8.GetBytes($"{{\"ts\":{ts}}}"),
            @event: "culpeo.ping",
            contentType: "application/json");

        await MessageHelper.SendControlFrameAsync(ws, pingFrame);
        var pong = await MessageHelper.ReceiveFrameAsync(ws);

        Assert.Equal("culpeo.pong", pong.Event);
        using var body = MessageHelper.ParseBody(pong);
        Assert.Equal(ts, body.RootElement.GetProperty("ts").GetInt64());
        Assert.True(body.RootElement.TryGetProperty("server_ts", out _));
    }

    // ── Server-to-client sends ─────────────────────────────────────────────────

    [Fact]
    public async Task Server_Can_Send_Media_To_Client_On_Output_Stream()
    {
        // Use a handler that sends a media frame right after connecting
        var sentTcs = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        string? outputStreamId = null;

        var handler = new CapturingHandler(onConnected: async session =>
        {
            // Find the output stream
            var stream = session.Streams.FirstOrDefault(s => s.Type == CulpeoStreamType.Output);
            if (stream is null)
            {
                sentTcs.SetException(new Exception("No output stream found."));
                return;
            }

            outputStreamId = stream.Id;
            await session.SendMediaAsync(stream.Id, new byte[] { 0xAA, 0xBB });
            sentTcs.SetResult();
        });

        using var server = CreateServer(handler);
        using var ws = await ConnectAsync(server);

        // Declare an output stream
        var bodyBytes = Encoding.UTF8.GetBytes(
            """{"version":"0.3","streams":[{"content_type":"audio/opus","type":"output","offset_type":"message"}]}""");

        var initFrame = new CulpeoMessage(
            CulpeoMessageKind.Control,
            bodyBytes,
            @event: "culpeo.init",
            authorization: "Bearer tok",
            contentType: "application/json");

        await MessageHelper.SendControlFrameAsync(ws, initFrame);
        var ack = await MessageHelper.ReceiveFrameAsync(ws);
        Assert.Equal("culpeo.init-ack", ack.Event);

        // Wait for server to send and then receive the media frame
        var mediaFrame = await MessageHelper.ReceiveFrameAsync(ws);
        Assert.Equal(CulpeoMessageKind.Media, mediaFrame.Kind);
        Assert.Equal(outputStreamId, mediaFrame.StreamId);
        Assert.Equal(2, mediaFrame.Body.Length);
    }

    // ── Concurrent sessions ────────────────────────────────────────────────────

    [Fact]
    public async Task Concurrent_Sessions_Are_Independent()
    {
        var connectedIds = new System.Collections.Concurrent.ConcurrentBag<string>();
        var handler = new CapturingHandler(
            onConnected: session => { connectedIds.Add(session.SessionId); });

        using var server = CreateServer(handler);

        const int sessionCount = 5;
        var tasks = Enumerable.Range(0, sessionCount).Select(async _ =>
        {
            var ws = await ConnectAsync(server);
            var ack = await MessageHelper.InitSessionAsync(ws);
            return ack.SessionId!;
        }).ToList();

        var ids = await Task.WhenAll(tasks);

        // All sessions should have distinct IDs
        Assert.Equal(sessionCount, ids.Distinct().Count());

        // Wait for all OnConnected callbacks
        await Task.Delay(500); // give handlers time to be called
        Assert.Equal(sessionCount, connectedIds.Distinct().Count());
    }

    // ── Stream count limit ────────────────────────────────────────────────────

    [Fact]
    public async Task Exceeding_MaxStreams_Returns_InitError_InvalidStreams()
    {
        using var server = CreateServer(
            new NoopHandler(),
            opt => opt.MaxStreamsPerSession = 2);

        using var ws = await ConnectAsync(server);

        var bodyBytes = Encoding.UTF8.GetBytes(
            """
            {"version":"0.3","streams":[
              {"content_type":"audio/opus","type":"input","purpose":"a","offset_type":"message"},
              {"content_type":"audio/opus","type":"input","purpose":"b","offset_type":"message"},
              {"content_type":"audio/opus","type":"input","purpose":"c","offset_type":"message"}
            ]}
            """);

        var initFrame = new CulpeoMessage(
            CulpeoMessageKind.Control,
            bodyBytes,
            @event: "culpeo.init",
            authorization: "Bearer tok",
            contentType: "application/json");

        await MessageHelper.SendControlFrameAsync(ws, initFrame);
        var response = await MessageHelper.ReceiveFrameAsync(ws);

        Assert.Equal("culpeo.init-error", response.Event);
        Assert.Equal("invalid-streams", response.Code);
    }

    // ── DI options validation ─────────────────────────────────────────────────

    [Fact]
    public void AddCulpeoStream_ValidatesAuthTimeoutCannotBeZero()
    {
        var builder = new WebHostBuilder()
            .UseEnvironment(Environments.Development)
            .ConfigureServices(services =>
            {
                services.AddRouting();
                services.AddCulpeoStream(opt => opt.AuthChallengeTimeout = TimeSpan.Zero);
            })
            .Configure(app => { app.UseRouting(); });

        using var server = new TestServer(builder);
        var ex = Assert.Throws<InvalidOperationException>(() =>
        {
            // Force DI resolution of CulpeoSessionServer which validates options
            _ = server.Services.GetRequiredService<CulpeoCore.CulpeoSessionServer>();
        });
        Assert.Contains("AuthChallengeTimeout", ex.Message);
    }

    // ── Private helpers ────────────────────────────────────────────────────────

    private static string ExtractFirstStreamId(CulpeoMessage ack)
    {
        using var body = MessageHelper.ParseBody(ack);
        return body.RootElement
            .GetProperty("streams")[0]
            .GetProperty("id")
            .GetString()!;
    }

    private sealed class CapturingHandler : ICulpeoStreamHandler
    {
        private readonly Func<ICulpeoStreamSession, Task>? _onConnected;
        private readonly Action<ICulpeoStreamSession, CulpeoMediaFrameContext>? _onMedia;
        private readonly Action<ICulpeoStreamSession, CulpeoEventContext>? _onEvent;
        private readonly Action<ICulpeoStreamSession, string?>? _onDisconnected;

        public CapturingHandler(
            Func<ICulpeoStreamSession, Task>? onConnected = null,
            Action<ICulpeoStreamSession, CulpeoMediaFrameContext>? onMedia = null,
            Action<ICulpeoStreamSession, CulpeoEventContext>? onEvent = null,
            Action<ICulpeoStreamSession, string?>? onDisconnected = null)
        {
            _onConnected = onConnected;
            _onMedia = onMedia;
            _onEvent = onEvent;
            _onDisconnected = onDisconnected;
        }

        // Convenience constructor accepting sync Action for onConnected
        public CapturingHandler(Action<ICulpeoStreamSession> onConnected)
            : this(onConnected: s => { onConnected(s); return Task.CompletedTask; })
        {
        }

        public Task OnConnectedAsync(ICulpeoStreamSession session, CancellationToken ct)
            => _onConnected?.Invoke(session) ?? Task.CompletedTask;

        public Task OnMediaFrameAsync(ICulpeoStreamSession session, CulpeoMediaFrameContext frame, CancellationToken ct)
        {
            _onMedia?.Invoke(session, frame);
            return Task.CompletedTask;
        }

        public Task OnEventAsync(ICulpeoStreamSession session, CulpeoEventContext @event, CancellationToken ct)
        {
            _onEvent?.Invoke(session, @event);
            return Task.CompletedTask;
        }

        public Task OnDisconnectedAsync(ICulpeoStreamSession session, string? closeCode, CancellationToken ct)
        {
            _onDisconnected?.Invoke(session, closeCode);
            return Task.CompletedTask;
        }
    }

    private sealed class NoopHandler : ICulpeoStreamHandler
    {
        public Task OnConnectedAsync(ICulpeoStreamSession session, CancellationToken ct) => Task.CompletedTask;
        public Task OnMediaFrameAsync(ICulpeoStreamSession session, CulpeoMediaFrameContext frame, CancellationToken ct) => Task.CompletedTask;
        public Task OnEventAsync(ICulpeoStreamSession session, CulpeoEventContext @event, CancellationToken ct) => Task.CompletedTask;
        public Task OnDisconnectedAsync(ICulpeoStreamSession session, string? closeCode, CancellationToken ct) => Task.CompletedTask;
    }
}
