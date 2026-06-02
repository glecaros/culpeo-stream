using System.Net;
using System.Text;
using CulpeoStream.Http2;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Hosting.Server;
using Microsoft.AspNetCore.Hosting.Server.Features;
using Microsoft.AspNetCore.Server.Kestrel.Core;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;

// Suppress CS0618 for AllowHttp2Cleartext throughout this test file:
// these tests intentionally exercise the cleartext h2c code path.
#pragma warning disable CS0618

namespace CulpeoStream.Http2.Tests;

/// <summary>
/// End-to-end integration tests for <see cref="CulpeoHttp2Client"/> and
/// the <see cref="CulpeoHttp2ServerExtensions.MapCulpeoHttp2"/> endpoint.
///
/// Uses a real in-process Kestrel server bound to a random loopback port so
/// that actual HTTP/2 framing and flow control are exercised.
///
/// Server is configured with <see cref="HttpProtocols.Http2"/> only (h2c).
/// TLS is skipped; the process-wide Http2UnencryptedSupport switch is enabled
/// once in the constructor. The 426 rejection test uses a separate HTTP/1.1
/// only server started inline.
/// </summary>
public sealed class Http2ClientServerTests : IAsyncLifetime
{
    // ── Test server infrastructure ────────────────────────────────────────────

    private WebApplication? _app;
    private string _baseUrl = string.Empty;

    /// <summary>
    /// Allows each test to install its own <see cref="ICulpeoHttp2Handler"/>
    /// without restarting the server.
    /// </summary>
    private readonly HandlerHolder _handlerHolder = new();

    /// <summary>
    /// Enables HTTP/2 cleartext support once per test class (process-wide switch).
    /// </summary>
    public Http2ClientServerTests()
    {
        AppContext.SetSwitch(
            "System.Net.Http.SocketsHttpHandler.Http2UnencryptedSupport",
            true);
    }

    public async Task InitializeAsync()
    {
        var builder = WebApplication.CreateBuilder();
        builder.Logging.SetMinimumLevel(LogLevel.Warning);

        // Bind to a random loopback port using HTTP/2 only (h2c).
        // Http1AndHttp2 on cleartext is not supported by Kestrel + SocketsHttpHandler
        // without TLS/ALPN — use Http2 only for the h2c prior-knowledge upgrade path.
        builder.WebHost.ConfigureKestrel(k =>
        {
            k.Listen(IPAddress.Loopback, 0, o =>
            {
                o.Protocols = HttpProtocols.Http2;
            });
        });

        _app = builder.Build();
        _app.MapCulpeoHttp2("/stream", _handlerHolder);
        await _app.StartAsync();

        // Retrieve the actual OS-assigned port via IServerAddressesFeature.
        // app.Urls contains the pre-binding URL (port may still be "0") until
        // IServerAddressesFeature.Addresses is updated post-bind.
        var server = _app.Services.GetRequiredService<IServer>();
        var feature = server.Features.Get<IServerAddressesFeature>()!;
        _baseUrl = feature.Addresses.First(); // e.g. "http://127.0.0.1:54321"
    }

    public async Task DisposeAsync()
    {
        if (_app is not null)
        {
            await _app.StopAsync();
            await _app.DisposeAsync();
        }
    }

    // ── Helper ────────────────────────────────────────────────────────────────

    private CulpeoHttp2Client CreateClient() =>
        new(new CulpeoHttp2ClientOptions { AllowHttp2Cleartext = true });

    private Uri StreamUri() => new(_baseUrl + "/stream");

    // ── Test 6: client sends control frame, server receives it ───────────────

    [Fact]
    public async Task Client_CanSendControlFrame_ServerReceivesIt()
    {
        var expected = Encoding.UTF8.GetBytes("Event: culpeo.ping\r\n\r\n{}");
        var receivedTcs = new TaskCompletionSource<(byte Type, byte[] Payload)>();

        _handlerHolder.Set(async (conn, ct) =>
        {
            var frame = await conn.ReceiveFrameAsync(ct);
            receivedTcs.TrySetResult(frame);
        });

        await using var client = CreateClient();
        await using var conn = await client.ConnectAsync(StreamUri());

        await conn.SendControlFrameAsync(expected);

        var (typeOctet, payload) = await receivedTcs.Task.WaitAsync(TimeSpan.FromSeconds(10));

        Assert.Equal(0x01, typeOctet);
        Assert.Equal(expected, payload);
    }

    // ── Test 7: client sends media frame, server receives it ─────────────────

    [Fact]
    public async Task Client_CanSendMediaFrame_ServerReceivesIt()
    {
        var expected = new byte[] { 0xDE, 0xAD, 0xBE, 0xEF };
        var receivedTcs = new TaskCompletionSource<(byte Type, byte[] Payload)>();

        _handlerHolder.Set(async (conn, ct) =>
        {
            var frame = await conn.ReceiveFrameAsync(ct);
            receivedTcs.TrySetResult(frame);
        });

        await using var client = CreateClient();
        await using var conn = await client.ConnectAsync(StreamUri());

        await conn.SendMediaFrameAsync(expected);

        var (typeOctet, payload) = await receivedTcs.Task.WaitAsync(TimeSpan.FromSeconds(10));

        Assert.Equal(0x02, typeOctet);
        Assert.Equal(expected, payload);
    }

    // ── Test 8: server sends control frame, client receives it ───────────────

    [Fact]
    public async Task Server_CanSendControlFrame_ClientReceivesIt()
    {
        var expected = Encoding.UTF8.GetBytes("Event: culpeo.init-ack\r\n\r\n{}");

        _handlerHolder.Set(async (conn, ct) =>
        {
            await conn.SendControlFrameAsync(expected, ct);
            // Keep the connection alive until the client disconnects.
            try { await conn.ReceiveFrameAsync(ct); } catch { /* EOF is expected */ }
        });

        await using var client = CreateClient();
        await using var conn = await client.ConnectAsync(StreamUri());

        var (typeOctet, payload) = await conn.ReceiveFrameAsync()
            .AsTask().WaitAsync(TimeSpan.FromSeconds(10));

        Assert.Equal(0x01, typeOctet);
        Assert.Equal(expected, payload);
    }

    // ── Test 9: server sends media frame, client receives it ─────────────────

    [Fact]
    public async Task Server_CanSendMediaFrame_ClientReceivesIt()
    {
        var expected = new byte[] { 0x01, 0x02, 0x03, 0x04 };

        _handlerHolder.Set(async (conn, ct) =>
        {
            await conn.SendMediaFrameAsync(expected, ct);
            try { await conn.ReceiveFrameAsync(ct); } catch { /* EOF */ }
        });

        await using var client = CreateClient();
        await using var conn = await client.ConnectAsync(StreamUri());

        var (typeOctet, payload) = await conn.ReceiveFrameAsync()
            .AsTask().WaitAsync(TimeSpan.FromSeconds(10));

        Assert.Equal(0x02, typeOctet);
        Assert.Equal(expected, payload);
    }

    // ── Test 10: bidirectional multi-frame exchange ───────────────────────────

    [Fact]
    public async Task Bidirectional_ExchangeMultipleFrames_AllArrive()
    {
        const int frameCount = 5;

        // Server: echo back every frame it receives (same type, same payload).
        _handlerHolder.Set(async (conn, ct) =>
        {
            for (var i = 0; i < frameCount; i++)
            {
                var (type, payload) = await conn.ReceiveFrameAsync(ct);
                if (type == 0x01)
                    await conn.SendControlFrameAsync(payload, ct);
                else
                    await conn.SendMediaFrameAsync(payload, ct);
            }
        });

        await using var client = CreateClient();
        await using var conn = await client.ConnectAsync(StreamUri());

        // Send alternating control and media frames.
        var sent = new List<(byte Type, byte[] Payload)>();
        for (var i = 0; i < frameCount; i++)
        {
            byte type = (byte)(i % 2 == 0 ? 0x01 : 0x02);
            var payload = Encoding.UTF8.GetBytes($"frame-{i}");
            sent.Add((type, payload));

            if (type == 0x01)
                await conn.SendControlFrameAsync(payload);
            else
                await conn.SendMediaFrameAsync(payload);
        }

        // Read echoed frames from the server.
        for (var i = 0; i < frameCount; i++)
        {
            var (type, payload) = await conn.ReceiveFrameAsync()
                .AsTask().WaitAsync(TimeSpan.FromSeconds(10));

            Assert.Equal(sent[i].Type, type);
            Assert.Equal(sent[i].Payload, payload);
        }
    }

    // ── Test 5.6: diagnostic — SendAsync deadlock check ───────────────────────

    [Fact]
    public async Task Diagnostic_SendAsync_ReturnsOnceServerHandlerYields()
    {
        // Verifies that once the server handler yields (awaits an async operation),
        // Kestrel can flush the buffered 200 OK headers and SendAsync returns.
        // A 500 ms window is generous — in practice it should be sub-millisecond.

        var serverYieldedTcs = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var allowServerToProceedTcs = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);

        _handlerHolder.Set(async (conn, ct) =>
        {
            // Yield so that the connection task can flush response headers
            await Task.Yield();
            serverYieldedTcs.TrySetResult();
            await allowServerToProceedTcs.Task.WaitAsync(TimeSpan.FromSeconds(10), ct);
        });

        AppContext.SetSwitch("System.Net.Http.SocketsHttpHandler.Http2UnencryptedSupport", true);
        using var handler = new System.Net.Http.SocketsHttpHandler { EnableMultipleHttp2Connections = true };
        using var httpClient = new HttpClient(handler);

        var doneSignal = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var content = new DiagnosticContent(
            onStreamAvailable: () => { },
            waitForDone: doneSignal.Task);

        var request = new HttpRequestMessage(HttpMethod.Post, StreamUri())
        {
            Content = content,
            Version = HttpVersion.Version20,
            VersionPolicy = HttpVersionPolicy.RequestVersionExact,
        };
        request.Headers.TryAddWithoutValidation("Culpeostream-Version", "1.0");

        var sendTask = httpClient.SendAsync(request, HttpCompletionOption.ResponseHeadersRead);

        // Wait for server to yield
        await serverYieldedTcs.Task.WaitAsync(TimeSpan.FromSeconds(5));

        // Give SocketsHttpHandler 500 ms to process the buffered 200 OK headers
        await Task.Delay(500);
        var sendReturnedAfterYield = sendTask.IsCompleted;

        // Unblock server and wait for send to complete
        allowServerToProceedTcs.TrySetResult();
        doneSignal.TrySetResult();
        var response = await sendTask.WaitAsync(TimeSpan.FromSeconds(10));
        response.Dispose();

        Assert.True(sendReturnedAfterYield,
            "SendAsync had NOT returned 200 OK within 500 ms of the server handler yielding. " +
            "Kestrel may not be flushing response headers independently of the request body.");
    }

    // ── Test 5.5: diagnostic — SerializeToStreamAsync timing ─────────────────

    [Fact]
    public async Task Diagnostic_SerializeToStreamAsync_StartsAfterSendAsync()
    {
        // This test verifies the ordering of SocketsHttpHandler's internal
        // HTTP/2 request-body task relative to SendAsync completion.
        // It does NOT use CulpeoHttp2Client — it uses raw HttpClient so we can
        // observe exactly when SerializeToStreamAsync is invoked.

        var streamSetTcs = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var doneSignal = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);

        var content = new DiagnosticContent(
            onStreamAvailable: () => streamSetTcs.TrySetResult(true),
            waitForDone: doneSignal.Task);

        _handlerHolder.Set((_, _) => Task.CompletedTask); // no-op handler

        AppContext.SetSwitch("System.Net.Http.SocketsHttpHandler.Http2UnencryptedSupport", true);
        using var handler = new System.Net.Http.SocketsHttpHandler { EnableMultipleHttp2Connections = true };
        using var httpClient = new HttpClient(handler);

        var request = new HttpRequestMessage(HttpMethod.Post, StreamUri())
        {
            Content = content,
            Version = HttpVersion.Version20,
            VersionPolicy = HttpVersionPolicy.RequestVersionExact,
        };
        request.Headers.TryAddWithoutValidation("Culpeostream-Version", "1.0");

        var t0 = DateTimeOffset.UtcNow;
        var response = await httpClient.SendAsync(request, HttpCompletionOption.ResponseHeadersRead);
        var t1 = DateTimeOffset.UtcNow;

        // Response headers received — now check if SerializeToStreamAsync started
        var streamStartedBeforeResponse = streamSetTcs.Task.IsCompleted;

        // Give SocketsHttpHandler up to 2 s to start the body task
        var started = await Task.WhenAny(streamSetTcs.Task, Task.Delay(2000)) == streamSetTcs.Task;
        var t2 = DateTimeOffset.UtcNow;

        doneSignal.TrySetResult(); // release SerializeToStreamAsync
        response.Dispose();

        // Report findings regardless of pass/fail so the test log shows the timing
        Assert.True(started,
            $"SerializeToStreamAsync was NOT called within 2 s of SendAsync returning. " +
            $"SendAsync took {(t1 - t0).TotalMilliseconds:F0} ms; " +
            $"timed out after {(t2 - t1).TotalMilliseconds:F0} ms.");

        // Useful observation:
        _ = streamStartedBeforeResponse; // logged implicitly in failure message
    }

    private sealed class DiagnosticContent : HttpContent
    {
        private readonly Action _onStreamAvailable;
        private readonly Task _waitForDone;

        public DiagnosticContent(Action onStreamAvailable, Task waitForDone)
        {
            _onStreamAvailable = onStreamAvailable;
            _waitForDone = waitForDone;
            Headers.ContentType = System.Net.Http.Headers.MediaTypeHeaderValue.Parse(
                "application/culpeostream");
        }

        protected override async Task SerializeToStreamAsync(
            Stream stream, TransportContext? context, CancellationToken ct)
        {
            _onStreamAvailable();
            try { await _waitForDone.WaitAsync(ct); }
            catch (OperationCanceledException) { }
        }

        protected override Task SerializeToStreamAsync(Stream stream, TransportContext? context)
            => SerializeToStreamAsync(stream, context, CancellationToken.None);

        protected override bool TryComputeLength(out long length) { length = -1; return false; }
    }

    // ── Test 11: server rejects HTTP/1.1 with 426 ────────────────────────────
    // This test starts its own HTTP/1.1-only Kestrel server so that the HTTP/1.1
    // request actually reaches the endpoint (an Http2-only port would reject the
    // connection at the Kestrel level before returning any HTTP status).

    [Fact]
    public async Task Server_RejectsHttp1_Returns426()
    {
        var http1Builder = WebApplication.CreateBuilder();
        http1Builder.Logging.SetMinimumLevel(LogLevel.Warning);
        http1Builder.WebHost.ConfigureKestrel(k =>
        {
            k.Listen(IPAddress.Loopback, 0, o =>
            {
                o.Protocols = HttpProtocols.Http1; // HTTP/1.1 only
            });
        });

        var http1App = http1Builder.Build();
        http1App.MapCulpeoHttp2("/stream", _handlerHolder);
        await http1App.StartAsync();

        var http1Server = http1App.Services.GetRequiredService<IServer>();
        var http1Address = http1Server.Features.Get<IServerAddressesFeature>()!.Addresses.First();

        try
        {
            _handlerHolder.Set((_, _) => Task.CompletedTask);

            // Regular HttpClient -> sends HTTP/1.1 (default for plain http://)
            using var httpClient = new HttpClient();
            var uri = new Uri(http1Address + "/stream");

            var request = new HttpRequestMessage(HttpMethod.Post, uri)
            {
                Content = new StringContent("", Encoding.UTF8, "application/culpeostream"),
            };
            request.Content.Headers.TryAddWithoutValidation("Culpeostream-Version", "1.0");

            var response = await httpClient.SendAsync(request);
            Assert.Equal(System.Net.HttpStatusCode.UpgradeRequired, response.StatusCode);
        }
        finally
        {
            await http1App.StopAsync();
            await http1App.DisposeAsync();
        }
    }

    // ── Test 12: cleartext opt-in works ──────────────────────────────────────

    [Fact]
    public async Task CleartextMode_ClientConnects_FrameExchangeSucceeds()
    {
        var expected = Encoding.UTF8.GetBytes("Event: test\r\n\r\n{}");
        var receivedTcs = new TaskCompletionSource<byte[]>();

        _handlerHolder.Set(async (conn, ct) =>
        {
            var (_, payload) = await conn.ReceiveFrameAsync(ct);
            receivedTcs.TrySetResult(payload);
        });

        // Explicitly exercise the AllowHttp2Cleartext = true code path.
        await using var client = new CulpeoHttp2Client(
            new CulpeoHttp2ClientOptions { AllowHttp2Cleartext = true });

        await using var conn = await client.ConnectAsync(StreamUri());
        await conn.SendControlFrameAsync(expected);

        var payload = await receivedTcs.Task.WaitAsync(TimeSpan.FromSeconds(10));
        Assert.Equal(expected, payload);
    }

    // ── Test 13: AuthorizationHeader is surfaced on server connection ─────────

    [Fact]
    public async Task AuthorizationHeader_IsAvailableOnServerConnection()
    {
        const string token = "Bearer test-token-abc";
        var capturedTcs = new TaskCompletionSource<string?>();

        _handlerHolder.Set((conn, _) =>
        {
            capturedTcs.TrySetResult(conn.AuthorizationHeader);
            return Task.CompletedTask;
        });

        await using var client = new CulpeoHttp2Client(new CulpeoHttp2ClientOptions
        {
            AllowHttp2Cleartext = true,
            AuthorizationHeader = token,
        });

        await using var conn = await client.ConnectAsync(StreamUri());

        // Dispose to flush END_STREAM so the server handler can run to completion.
        await conn.DisposeAsync();

        var captured = await capturedTcs.Task.WaitAsync(TimeSpan.FromSeconds(10));
        Assert.Equal(token, captured);
    }

    // ── HandlerHolder helper ──────────────────────────────────────────────────

    /// <summary>
    /// Thread-safe per-test handler mux — allows each test to install its own
    /// <see cref="ICulpeoHttp2Handler"/> implementation without restarting the server.
    /// </summary>
    private sealed class HandlerHolder : ICulpeoHttp2Handler
    {
        // volatile field swap is sufficient — no awaited critical section here;
        // tests set the handler before making connections.
        private volatile Func<CulpeoHttp2ServerConnection, CancellationToken, Task>? _impl;

        public void Set(Func<CulpeoHttp2ServerConnection, CancellationToken, Task> impl)
            => _impl = impl;

        public Task HandleAsync(CulpeoHttp2ServerConnection connection, CancellationToken ct)
        {
            var impl = _impl ?? throw new InvalidOperationException("No handler installed for this test.");
            return impl(connection, ct);
        }
    }
}
