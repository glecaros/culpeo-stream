using System.Net;
using System.Net.Http.Headers;
using System.Net.Security;
using System.Security.Authentication;

namespace CulpeoStream.Http2;

// ── Internal helpers ──────────────────────────────────────────────────────────

/// <summary>
/// Custom <see cref="HttpContent"/> for true HTTP/2 duplex streaming.
///
/// <para>
/// SocketsHttpHandler calls <see cref="SerializeToStreamAsync"/> on a
/// background task (fire-and-forget) before the response headers task
/// completes.  This implementation captures the raw HTTP/2 request-body
/// stream via <see cref="StreamAvailableTask"/> and then keeps that stream
/// open until <see cref="Complete"/> is called (i.e. when the connection
/// is disposed).
/// </para>
/// <para>
/// Rationale: an intermediate <c>Pipe</c> introduces a layer where buffered
/// data can sit unread if <c>SerializeToStreamAsync</c> has not yet started
/// its <c>CopyToAsync</c> loop — observed as server-side <c>ReadAsync</c>
/// blocking indefinitely even after the client wrote frames.  Exposing the
/// raw stream directly eliminates that intermediate buffer and the associated
/// race condition.
/// </para>
/// </summary>
internal sealed class RequestBodyContent : HttpContent
{
    // RunContinuationsAsynchronously prevents SerializeToStreamAsync from being
    // resumed synchronously on the TCS-setter's thread — that thread is the
    // SocketsHttpHandler internal task, which must not be blocked.
    private readonly TaskCompletionSource<Stream> _streamTcs =
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    private readonly TaskCompletionSource _doneTcs =
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    internal RequestBodyContent()
    {
        Headers.ContentType = MediaTypeHeaderValue.Parse("application/culpeostream");
    }

    /// <summary>
    /// A task that completes with the raw HTTP/2 request-body stream once
    /// <see cref="SerializeToStreamAsync"/> has been invoked by
    /// <c>SocketsHttpHandler</c>.
    /// </summary>
    internal Task<Stream> StreamAvailableTask => _streamTcs.Task;

    /// <summary>
    /// Signals that the caller is done writing; causes
    /// <see cref="SerializeToStreamAsync"/> to return, which makes
    /// SocketsHttpHandler send END_STREAM on the request body.
    /// </summary>
    internal void Complete() => _doneTcs.TrySetResult();

    // Called by SocketsHttpHandler on a background task.
    // 1. Publishes the stream so callers can write frames directly.
    // 2. Blocks until Complete() is called.
    protected override async Task SerializeToStreamAsync(
        Stream stream,
        TransportContext? context,
        CancellationToken ct)
    {
        // Publish the stream — after this point callers can write to it.
        _streamTcs.TrySetResult(stream);

        // Hold the request body open until DisposeAsync signals done.
        // Use WaitAsync so an external CancellationToken can abort the wait.
        try
        {
            await _doneTcs.Task.WaitAsync(ct).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            // Cancellation means the connection is being torn down.
            // Returning from SerializeToStreamAsync causes SocketsHttpHandler
            // to send END_STREAM on the request, which is the correct behaviour.
        }
    }

    // Legacy override (called by older HttpClient versions without CT).
    protected override Task SerializeToStreamAsync(Stream stream, TransportContext? context)
        => SerializeToStreamAsync(stream, context, CancellationToken.None);

    protected override bool TryComputeLength(out long length)
    {
        length = -1; // unknown / streaming
        return false;
    }
}

/// <summary>
/// Configuration options for <see cref="CulpeoHttp2Client"/>.
/// </summary>
public sealed class CulpeoHttp2ClientOptions
{
    /// <summary>
    /// Allow plain-text HTTP/2 (h2c). Must only be used in local development.
    /// In production, always use <c>https://</c>.
    /// Default: <see langword="false"/>.
    /// </summary>
    /// <remarks>
    /// When set to <see langword="true"/>, the client calls
    /// <c>AppContext.SetSwitch("System.Net.Http.SocketsHttpHandler.Http2UnencryptedSupport", true)</c>.
    /// This switch is process-wide and irreversible for the lifetime of the process.
    /// </remarks>
    // SEC-031: produce a compile-time warning at every call site that sets this property.
    [Obsolete(
        "AllowHttp2Cleartext must not be used in production. " +
        "Set this property only in development/test environments.",
        error: false)]
    public bool AllowHttp2Cleartext { get; init; } = false;

    /// <summary>Maximum payload size accepted from the server. Default: 16 MiB.</summary>
    public int MaxPayloadBytes { get; init; } = Http2FrameReader.DefaultMaxPayloadBytes;

    /// <summary>
    /// Optional value for the <c>Authorization</c> HTTP request header,
    /// e.g. <c>"Bearer ey…"</c>.  Never logged or reflected in exceptions.
    /// </summary>
    public string? AuthorizationHeader { get; init; }
}

/// <summary>
/// Represents an active bidirectional CulpeoStream-over-HTTP/2 connection
/// established by <see cref="CulpeoHttp2Client.ConnectAsync"/>.
/// </summary>
/// <remarks>
/// Thread-safety: <see cref="SendControlFrameAsync"/> and
/// <see cref="SendMediaFrameAsync"/> may be called concurrently.
/// <see cref="ReceiveFrameAsync"/> must be consumed by a single reader.
/// </remarks>
public sealed class CulpeoHttp2Connection : IAsyncDisposable
{
    // The raw HTTP/2 request-body stream — written to directly for outgoing frames.
    private readonly Stream _requestBodyStream;
    private readonly Stream _responseStream;
    private readonly HttpResponseMessage _response;
    private readonly RequestBodyContent _requestContent;
    private readonly int _maxPayloadBytes;

    // SemaphoreSlim(1,1): the critical section (write type+length+payload+flush)
    // spans multiple awaits, so a plain lock() would deadlock.  Atomic
    // Interlocked is insufficient because we need to queue waiters.
    private readonly SemaphoreSlim _sendLock = new(1, 1);

    // Disposal guard — Interlocked.CompareExchange prevents double-dispose.
    private int _disposed;

    internal CulpeoHttp2Connection(
        Stream requestBodyStream,
        Stream responseStream,
        HttpResponseMessage response,
        RequestBodyContent requestContent,
        int maxPayloadBytes)
    {
        _requestBodyStream = requestBodyStream;
        _responseStream = responseStream;
        _response = response;
        _requestContent = requestContent;
        _maxPayloadBytes = maxPayloadBytes;
    }

    /// <summary>Sends a control frame (type <c>0x01</c>) to the server.</summary>
    public ValueTask SendControlFrameAsync(ReadOnlyMemory<byte> payload, CancellationToken ct = default)
        => SendLockedAsync(0x01, payload, ct);

    /// <summary>Sends a media frame (type <c>0x02</c>) to the server.</summary>
    public ValueTask SendMediaFrameAsync(ReadOnlyMemory<byte> payload, CancellationToken ct = default)
        => SendLockedAsync(0x02, payload, ct);

    /// <summary>
    /// Receives the next frame from the server.
    /// </summary>
    /// <returns>
    /// A tuple of the type octet (<c>0x01</c> or <c>0x02</c>) and the payload bytes.
    /// </returns>
    /// <exception cref="EndOfStreamException">Server closed the stream.</exception>
    public ValueTask<(byte TypeOctet, byte[] Payload)> ReceiveFrameAsync(CancellationToken ct = default)
        => Http2FrameReader.ReadFrameAsync(_responseStream, _maxPayloadBytes, ct);

    // ── IAsyncDisposable ──────────────────────────────────────────────────────

    /// <summary>
    /// Completes the outgoing request body (signals EOF to the server) and
    /// disposes the response and send lock.
    /// </summary>
    public async ValueTask DisposeAsync()
    {
        // Atomic guard: only the first caller enters the dispose body.
        if (Interlocked.CompareExchange(ref _disposed, 1, 0) != 0) return;

        // Signal SerializeToStreamAsync to return → SocketsHttpHandler sends
        // END_STREAM on the request body → server sees EOF on Request.Body.
        _requestContent.Complete();

        try
        {
            await _requestBodyStream.FlushAsync().ConfigureAwait(false);
        }
        catch { /* best-effort */ }

        try
        {
            _responseStream.Dispose();
        }
        catch { /* best-effort */ }

        _response.Dispose();
        _sendLock.Dispose();
    }

    // ── Private helpers ───────────────────────────────────────────────────────

    private async ValueTask SendLockedAsync(byte typeOctet, ReadOnlyMemory<byte> payload, CancellationToken ct)
    {
        // Serialize frame writes: WriteFrameAsync spans multiple awaits
        // (header write, payload write, flush), so SemaphoreSlim is required.
        await _sendLock.WaitAsync(ct).ConfigureAwait(false);
        try
        {
            await Http2FrameWriter.WriteFrameAsync(_requestBodyStream, typeOctet, payload, ct)
                .ConfigureAwait(false);
        }
        finally
        {
            _sendLock.Release();
        }
    }
}

/// <summary>
/// CulpeoStream client that connects to a server over HTTP/2 POST,
/// establishing a bidirectional frame stream.
/// </summary>
/// <remarks>
/// Reuse a single <see cref="CulpeoHttp2Client"/> instance across multiple
/// <see cref="ConnectAsync"/> calls — the underlying <see cref="HttpClient"/>
/// supports HTTP/2 connection pooling.
/// </remarks>
public sealed class CulpeoHttp2Client : IAsyncDisposable
{
    private readonly CulpeoHttp2ClientOptions _options;
    private readonly HttpClient _httpClient;

    // Disposal guard
    private int _disposed;

    /// <summary>
    /// Initialises the client with the given <paramref name="options"/>.
    /// </summary>
    /// <remarks>
    /// When <see cref="CulpeoHttp2ClientOptions.AllowHttp2Cleartext"/> is
    /// <see langword="true"/>, enables the process-wide
    /// <c>Http2UnencryptedSupport</c> switch.
    /// </remarks>
#pragma warning disable CA2000  // handler lifetime managed by HttpClient
#pragma warning disable CS0618  // AllowHttp2Cleartext: internal implementation access — [Obsolete] targets call sites
    public CulpeoHttp2Client(CulpeoHttp2ClientOptions options)
    {
        _options = options ?? throw new ArgumentNullException(nameof(options));

        if (options.AllowHttp2Cleartext)
        {
            // Process-wide switch; safe to call multiple times.
            AppContext.SetSwitch(
                "System.Net.Http.SocketsHttpHandler.Http2UnencryptedSupport",
                true);

            // SEC-031: warn visibly — this switch is process-wide, irreversible,
            // and must never be set in production.
            Console.Error.WriteLine(
                "[CulpeoStream] WARNING: Http2UnencryptedSupport is enabled. " +
                "Cleartext HTTP/2 must only be used in local development. " +
                "This switch is process-wide and cannot be reverted.");
        }

        var handler = new SocketsHttpHandler
        {
            SslOptions = new SslClientAuthenticationOptions
            {
                EnabledSslProtocols = SslProtocols.Tls12 | SslProtocols.Tls13
            },
            AllowAutoRedirect = false,
            // Allow the handler to open multiple HTTP/2 connections to the
            // same origin if the server's SETTINGS_MAX_CONCURRENT_STREAMS
            // is low — beneficial for multi-session use-cases (Addendum C.8).
            EnableMultipleHttp2Connections = true,
        };

        _httpClient = new HttpClient(handler);
    }
#pragma warning restore CS0618
#pragma warning restore CA2000

    /// <summary>
    /// Opens an HTTP/2 POST connection to <paramref name="endpoint"/> and
    /// returns a <see cref="CulpeoHttp2Connection"/> ready for bidirectional
    /// frame exchange.
    /// </summary>
    /// <exception cref="InvalidOperationException">
    /// Thrown when an <c>http://</c> URI is supplied and
    /// <see cref="CulpeoHttp2ClientOptions.AllowHttp2Cleartext"/> is
    /// <see langword="false"/>.
    /// </exception>
    /// <exception cref="HttpRequestException">
    /// Thrown when the server returns a non-2xx status code.
    /// </exception>
    public async Task<CulpeoHttp2Connection> ConnectAsync(Uri endpoint, CancellationToken ct = default)
    {
        ArgumentNullException.ThrowIfNull(endpoint);

#pragma warning disable CS0618  // AllowHttp2Cleartext: internal implementation access
        if (!_options.AllowHttp2Cleartext &&
            string.Equals(endpoint.Scheme, "http", StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException(
                "CulpeoStream HTTP/2 transport requires HTTPS. " +
                "Set AllowHttp2Cleartext = true to allow plain-text HTTP/2 (development only).");
        }
#pragma warning restore CS0618

        // RequestBodyContent captures the raw HTTP/2 request-body stream via
        // a TaskCompletionSource inside SerializeToStreamAsync.  This avoids
        // an intermediate Pipe and the race condition where PipeReader.CopyToAsync
        // hasn't started yet when the caller writes the first frame.
        var requestContent = new RequestBodyContent();

        var request = new HttpRequestMessage(HttpMethod.Post, endpoint)
        {
            Content = requestContent,
            Version = HttpVersion.Version20,
            VersionPolicy = HttpVersionPolicy.RequestVersionExact,
        };

        // Culpeostream-Version is an application-level header (Addendum C.2).
        // Added to request.Headers (not content headers) to guarantee it is
        // always included in the HTTP/2 HEADERS frame regardless of the content
        // type.  Non-standard headers added to HttpContent.Headers may not be
        // forwarded by some HttpClient versions.
        request.Headers.TryAddWithoutValidation("Culpeostream-Version", "1.0");

        if (!string.IsNullOrEmpty(_options.AuthorizationHeader))
        {
            // TryAddWithoutValidation avoids parsing/validation of the token
            // value; also ensures we never throw on unusual bearer formats.
            request.Headers.TryAddWithoutValidation("Authorization", _options.AuthorizationHeader);
        }

        // Start the POST — with ResponseHeadersRead, SocketsHttpHandler fires
        // SerializeToStreamAsync on a background task AFTER returning the
        // HttpResponseMessage (once the server's 200 OK headers are received).
        // We therefore await the response first, then wait for the request-body
        // stream to be published by the background task.
        HttpResponseMessage response;
        try
        {
            response = await _httpClient
                .SendAsync(request, HttpCompletionOption.ResponseHeadersRead, ct)
                .ConfigureAwait(false);
        }
        catch
        {
            requestContent.Complete();
            throw;
        }

        if (!response.IsSuccessStatusCode)
        {
            var status = (int)response.StatusCode;
            response.Dispose();
            requestContent.Complete();
            throw new HttpRequestException(
                $"Server rejected CulpeoStream HTTP/2 connection with status {status}.");
        }

        // Wait for SerializeToStreamAsync to publish the request-body stream.
        // SocketsHttpHandler's background body-send task starts shortly after
        // SendAsync returns — typically within a tick or two.
        // 30-second timeout to surface hangs as TimeoutException instead of
        // blocking forever during diagnosis.
        Stream requestBodyStream;
        try
        {
            requestBodyStream = await requestContent.StreamAvailableTask
                .WaitAsync(TimeSpan.FromSeconds(30), ct)
                .ConfigureAwait(false);
        }
        catch
        {
            response.Dispose();
            requestContent.Complete();
            throw;
        }

        var responseStream = await response.Content
            .ReadAsStreamAsync(ct)
            .ConfigureAwait(false);

        return new CulpeoHttp2Connection(
            requestBodyStream,
            responseStream,
            response,
            requestContent,
            _options.MaxPayloadBytes);
    }

    // ── IAsyncDisposable ──────────────────────────────────────────────────────

    public ValueTask DisposeAsync()
    {
        if (Interlocked.CompareExchange(ref _disposed, 1, 0) != 0) return ValueTask.CompletedTask;
        _httpClient.Dispose();
        return ValueTask.CompletedTask;
    }
}
