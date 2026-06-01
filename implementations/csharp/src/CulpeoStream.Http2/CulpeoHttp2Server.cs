using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Http.Features;
using Microsoft.AspNetCore.Routing;
using Microsoft.AspNetCore.Server.Kestrel.Core.Features;

namespace CulpeoStream.Http2;

/// <summary>
/// Application handler for a single CulpeoStream HTTP/2 connection.
/// Implement this interface and register it via
/// <see cref="CulpeoHttp2ServerExtensions.MapCulpeoHttp2"/>.
/// </summary>
public interface ICulpeoHttp2Handler
{
    /// <summary>
    /// Invoked once per HTTP/2 request that passes all transport-level checks.
    /// The implementation exchanges frames through <paramref name="connection"/>
    /// until it is done, then returns — the response stream is closed on return.
    /// </summary>
    Task HandleAsync(CulpeoHttp2ServerConnection connection, CancellationToken ct);
}

/// <summary>
/// Server-side view of an active CulpeoStream HTTP/2 connection.
/// Wraps the ASP.NET Core <see cref="HttpContext"/> and exposes a
/// frame-oriented send/receive API that mirrors <c>CulpeoHttp2Connection</c>
/// on the client side.
/// </summary>
/// <remarks>
/// Thread-safety: <see cref="SendControlFrameAsync"/> and
/// <see cref="SendMediaFrameAsync"/> may be called concurrently.
/// <see cref="ReceiveFrameAsync"/> must be consumed by a single reader.
/// </remarks>
public sealed class CulpeoHttp2ServerConnection
{
    private readonly Stream _requestStream;
    private readonly Stream _responseStream;
    private readonly int _maxPayloadBytes;

    // SemaphoreSlim(1,1): serialize frame writes because WriteFrameAsync
    // spans multiple awaits (write header, write payload, flush).
    // A plain lock() cannot be used around async code.
    private readonly SemaphoreSlim _sendLock = new(1, 1);

    internal CulpeoHttp2ServerConnection(
        Stream requestStream,
        Stream responseStream,
        string? authorizationHeader,
        int maxPayloadBytes)
    {
        _requestStream = requestStream;
        _responseStream = responseStream;
        AuthorizationHeader = authorizationHeader;
        _maxPayloadBytes = maxPayloadBytes;
    }

    /// <summary>
    /// The raw value of the <c>Authorization</c> HTTP header sent by the
    /// client in the POST request, or <see langword="null"/> if omitted.
    /// Never logged or reflected in exceptions.
    /// </summary>
    public string? AuthorizationHeader { get; }

    /// <summary>Sends a control frame (type <c>0x01</c>) to the client.</summary>
    public ValueTask SendControlFrameAsync(
        ReadOnlyMemory<byte> payload,
        CancellationToken ct = default)
        => SendLockedAsync(0x01, payload, ct);

    /// <summary>Sends a media frame (type <c>0x02</c>) to the client.</summary>
    public ValueTask SendMediaFrameAsync(
        ReadOnlyMemory<byte> payload,
        CancellationToken ct = default)
        => SendLockedAsync(0x02, payload, ct);

    /// <summary>
    /// Receives the next frame from the client.
    /// </summary>
    /// <exception cref="EndOfStreamException">Client closed the stream.</exception>
    public ValueTask<(byte TypeOctet, byte[] Payload)> ReceiveFrameAsync(
        CancellationToken ct = default)
        => Http2FrameReader.ReadFrameAsync(_requestStream, _maxPayloadBytes, ct);

    // ── Private helpers ───────────────────────────────────────────────────────

    private async ValueTask SendLockedAsync(
        byte typeOctet,
        ReadOnlyMemory<byte> payload,
        CancellationToken ct)
    {
        await _sendLock.WaitAsync(ct).ConfigureAwait(false);
        try
        {
            await Http2FrameWriter.WriteFrameAsync(_responseStream, typeOctet, payload, ct)
                .ConfigureAwait(false);
        }
        finally
        {
            _sendLock.Release();
        }
    }
}

/// <summary>
/// Extension methods for registering a CulpeoStream HTTP/2 endpoint in an
/// ASP.NET Core minimal-API application.
/// </summary>
public static class CulpeoHttp2ServerExtensions
{
    /// <summary>Default maximum payload size passed to the server connection.</summary>
    private const int DefaultMaxPayloadBytes = Http2FrameReader.DefaultMaxPayloadBytes;

    /// <summary>
    /// Maps an HTTP POST endpoint at <paramref name="pattern"/> that accepts
    /// CulpeoStream HTTP/2 connections and dispatches them to
    /// <paramref name="handler"/>.
    /// </summary>
    /// <remarks>
    /// <list type="bullet">
    ///   <item>Returns 426 for non-HTTP/2 requests.</item>
    ///   <item>Returns 400 if the mandatory <c>Content-Type: application/culpeostream</c>
    ///   or <c>Culpeostream-Version</c> headers are absent.</item>
    ///   <item>Disables synchronous I/O on the response body
    ///   (<see cref="IHttpBodyControlFeature"/>).</item>
    ///   <item>Calls <c>Response.StartAsync</c> immediately so the client's
    ///   <c>SendAsync(ResponseHeadersRead)</c> can return before any
    ///   application frames are exchanged.</item>
    /// </list>
    /// </remarks>
    public static IEndpointRouteBuilder MapCulpeoHttp2(
        this IEndpointRouteBuilder app,
        string pattern,
        ICulpeoHttp2Handler handler,
        int maxPayloadBytes = DefaultMaxPayloadBytes)
    {
        ArgumentNullException.ThrowIfNull(app);
        ArgumentNullException.ThrowIfNull(handler);
        ArgumentException.ThrowIfNullOrEmpty(pattern);

        app.MapPost(pattern, async (HttpContext ctx) =>
        {
            // ── Require HTTP/2 (Addendum C.5) ────────────────────────────────
            if (ctx.Request.Protocol is not "HTTP/2")
            {
                ctx.Response.StatusCode = StatusCodes.Status426UpgradeRequired;
                ctx.Response.Headers["Upgrade"] = "h2";
                await ctx.Response.WriteAsync(
                    "CulpeoStream HTTP/2 transport requires HTTP/2.",
                    ctx.RequestAborted)
                    .ConfigureAwait(false);
                return;
            }

            // ── Validate required request headers (Addendum C.2) ─────────────
            var contentType = ctx.Request.ContentType;
            if (!string.Equals(contentType, "application/culpeostream",
                    StringComparison.OrdinalIgnoreCase))
            {
                ctx.Response.StatusCode = StatusCodes.Status400BadRequest;
                await ctx.Response.WriteAsync(
                    "Missing or incorrect Content-Type header. Expected: application/culpeostream.",
                    ctx.RequestAborted)
                    .ConfigureAwait(false);
                return;
            }

            if (!ctx.Request.Headers.TryGetValue("Culpeostream-Version", out _))
            {
                ctx.Response.StatusCode = StatusCodes.Status400BadRequest;
                await ctx.Response.WriteAsync(
                    "Missing required Culpeostream-Version header.",
                    ctx.RequestAborted)
                    .ConfigureAwait(false);
                return;
            }

            // ── Disable synchronous I/O (best practice for HTTP/2 streaming) ─
            var bodyControl = ctx.Features.Get<IHttpBodyControlFeature>();
            if (bodyControl is not null)
            {
                bodyControl.AllowSynchronousIO = false;
            }

            // ── Disable minimum request-body data rate ────────────────────────
            // CulpeoStream is a long-lived bidirectional protocol: the client
            // may not send frames for seconds at a time (e.g., between audio
            // bursts).  Kestrel's default MinRequestBodyDataRate (240 B/s with
            // 5 s grace period) would close such connections prematurely.
            var rateFeature = ctx.Features.Get<IHttpMinRequestBodyDataRateFeature>();
            if (rateFeature is not null)
            {
                rateFeature.MinDataRate = null;
            }

            // ── Send 200 OK headers immediately ───────────────────────────────
            // This allows the client's SendAsync(ResponseHeadersRead) to return
            // before any CulpeoStream frames are exchanged.
            ctx.Response.StatusCode = StatusCodes.Status200OK;
            ctx.Response.ContentType = "application/culpeostream";
            await ctx.Response.StartAsync(ctx.RequestAborted).ConfigureAwait(false);

            // Flush an empty body chunk so SocketsHttpHandler receives the
            // response HEADERS frame and unblocks SendAsync(ResponseHeadersRead)
            // even before any application data is written.  Without this flush,
            // SocketsHttpHandler may buffer the headers and defer delivery until
            // the response body (or trailers) are available.
            await ctx.Response.Body.FlushAsync(ctx.RequestAborted).ConfigureAwait(false);

            // ── Dispatch to application handler ───────────────────────────────
            var authHeader = ctx.Request.Headers.Authorization.FirstOrDefault();
            var connection = new CulpeoHttp2ServerConnection(
                ctx.Request.Body,
                ctx.Response.Body,
                authHeader,
                maxPayloadBytes);

            try
            {
                await handler.HandleAsync(connection, ctx.RequestAborted)
                    .ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (ctx.RequestAborted.IsCancellationRequested)
            {
                // Normal cancellation — client disconnected or request was aborted.
            }
            finally
            {
                // Flush any remaining buffered response data and send END_STREAM.
                try
                {
                    await ctx.Response.CompleteAsync().ConfigureAwait(false);
                }
                catch { /* best-effort */ }
            }
        });

        return app;
    }
}
