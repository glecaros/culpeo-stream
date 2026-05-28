using System.Net.WebSockets;
using System.Text;
using CulpeoStream.Core;
using Microsoft.Extensions.Logging;

namespace CulpeoStream.AspNetCore.Internal;

/// <summary>
/// Bridges a <see cref="WebSocket"/> connection to <see cref="CulpeoConnection"/>
/// from CulpeoStream.Core and dispatches lifecycle events to an
/// <see cref="ICulpeoStreamHandler"/>.
///
/// Implements <see cref="ICulpeoStreamSession"/> so the handler can send
/// outbound media and events without needing direct access to the WebSocket.
///
/// Thread-safety: <see cref="SendMediaAsync"/> and <see cref="SendEventAsync"/>
/// may be called concurrently with the receive loop. A <see cref="SemaphoreSlim"/>
/// serialises all WebSocket sends.
/// </summary>
internal sealed class WebSocketTransportAdapter : ICulpeoStreamSession
{
    private readonly WebSocket _ws;
    private readonly CulpeoConnection _connection;
    private readonly ICulpeoStreamHandler _handler;
    private readonly CulpeoStreamOptions _options;
    private readonly ILogger _logger;
    private readonly CulpeoFrameParser _parser = new();
    private readonly CulpeoFrameSerializer _serializer = new();
    private readonly SemaphoreSlim _sendLock = new(1, 1);

    private DateTimeOffset _sessionStart;

    // ICulpeoStreamSession is only valid while the session is Established; we
    // expose it to the handler before that could theoretically happen.
    private volatile bool _closed;

    public WebSocketTransportAdapter(
        WebSocket ws,
        CulpeoConnection connection,
        ICulpeoStreamHandler handler,
        CulpeoStreamOptions options,
        ILogger logger)
    {
        _ws = ws;
        _connection = connection;
        _handler = handler;
        _options = options;
        _logger = logger;
    }

    // ── ICulpeoStreamSession ──────────────────────────────────────────────────

    public string SessionId => _connection.SessionId
        ?? throw new InvalidOperationException("Session is not yet established.");

    public IReadOnlyList<CulpeoStreamInfo> Streams => _connection.Streams;

    public async Task SendMediaAsync(
        string streamId,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(_closed, this);

        var timestamp = ComputeTimestampMicros();
        var frame = await _connection.SendMediaAsync(streamId, payload, timestamp, cancellationToken)
            .ConfigureAwait(false);

        await SendFrameAsync(frame, cancellationToken).ConfigureAwait(false);
    }

    public async Task SendEventAsync(
        string eventName,
        string? streamId = null,
        string jsonBody = "{}",
        CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(_closed, this);

        if (string.IsNullOrEmpty(eventName))
        {
            throw new ArgumentException("Event name must not be null or empty.", nameof(eventName));
        }

        if (eventName.StartsWith("culpeo.", StringComparison.Ordinal))
        {
            throw new ArgumentException(
                "Application events must not use the reserved 'culpeo.' namespace.", nameof(eventName));
        }

        var bodyBytes = Encoding.UTF8.GetBytes(string.IsNullOrWhiteSpace(jsonBody) ? "{}" : jsonBody);
        var frame = new CulpeoFrame(
            CulpeoFrameKind.Control,
            bodyBytes,
            @event: eventName,
            contentType: "application/json",
            streamId: streamId);

        await SendFrameAsync(frame, cancellationToken).ConfigureAwait(false);
    }

    public async Task CloseAsync(
        string code = "normal",
        string reason = "Session closed",
        CancellationToken cancellationToken = default)
    {
        if (_closed)
        {
            return;
        }

        var closeFrame = BuildCloseFrame(code, reason);
        await SendFrameAsync(closeFrame, cancellationToken).ConfigureAwait(false);
    }

    // ── Main run loop ─────────────────────────────────────────────────────────

    /// <summary>
    /// Runs the CulpeoStream protocol loop until the session ends.
    /// Returns once the session is fully closed and the WebSocket has been
    /// gracefully closed where possible.
    /// </summary>
    public async Task RunAsync(CancellationToken cancellationToken)
    {
        bool handlerConnected = false;
        string? closeCode = null;

        try
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                // ── Build a per-receive idle-timeout token ─────────────────
                using var idleCts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
                idleCts.CancelAfter(_options.IdleTimeout);

                // ── Receive next WebSocket message ─────────────────────────
                byte[] messageBytes;
                WebSocketMessageType messageType;
                try
                {
                    (messageType, messageBytes) = await ReceiveMessageAsync(idleCts.Token)
                        .ConfigureAwait(false);
                }
                catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
                {
                    // Idle timeout fired
                    _logger.LogInformation("Session idle timeout for session {SessionId}.", _connection.SessionId);
                    var idleClose = BuildCloseFrame("idle-timeout", "Session closed due to inactivity.");
                    await SendFrameAsync(idleClose, CancellationToken.None).ConfigureAwait(false);
                    closeCode = "idle-timeout";
                    break;
                }

                if (messageType == WebSocketMessageType.Close)
                {
                    // Client initiated transport close
                    _logger.LogDebug("WebSocket close frame received for session {SessionId}.", _connection.SessionId);
                    break;
                }

                // ── Parse frame ────────────────────────────────────────────
                CulpeoFrame frame;
                try
                {
                    var kind = messageType == WebSocketMessageType.Text
                        ? CulpeoFrameKind.Control
                        : CulpeoFrameKind.Media;

                    frame = await _parser.ParseAsync(messageBytes, kind, cancellationToken)
                        .ConfigureAwait(false);
                }
                catch (FormatException ex)
                {
                    _logger.LogWarning("Malformed frame for session {SessionId}: {Message}",
                        _connection.SessionId, ex.Message);

                    var parseErrorClose = BuildCloseFrame("protocol-error", "Malformed frame: " + ex.Message);
                    await SendFrameAsync(parseErrorClose, CancellationToken.None).ConfigureAwait(false);
                    closeCode = "protocol-error";
                    break;
                }

                // ── Process frame through Core state machine ───────────────
                var result = await _connection.ReceiveAsync(frame, cancellationToken)
                    .ConfigureAwait(false);

                // ── Send all outbound frames generated by Core ─────────────
                foreach (var outbound in result.OutboundFrames)
                {
                    await SendFrameAsync(outbound, cancellationToken).ConfigureAwait(false);
                }

                // ── Session established: call OnConnectedAsync exactly once ─
                if (!handlerConnected && result.State == CulpeoSessionState.Established)
                {
                    handlerConnected = true;
                    _sessionStart = DateTimeOffset.UtcNow;
                    await _handler.OnConnectedAsync(this, cancellationToken).ConfigureAwait(false);
                }

                // ── Dispatch application-level frames to handler ───────────
                if (result.State == CulpeoSessionState.Established && !result.ShouldClose)
                {
                    if (frame.Kind == CulpeoFrameKind.Media)
                    {
                        var ctx = new CulpeoMediaFrameContext(
                            frame.StreamId!,
                            frame.Offset ?? 0,
                            frame.Timestamp,
                            frame.ContentType ?? string.Empty,
                            frame.Body);

                        await _handler.OnMediaFrameAsync(this, ctx, cancellationToken)
                            .ConfigureAwait(false);
                    }
                    else if (frame.Kind == CulpeoFrameKind.Control && IsApplicationEvent(frame.Event))
                    {
                        var ctx = new CulpeoEventContext(
                            frame.Event!,
                            frame.StreamId,
                            frame.Body.IsEmpty ? "{}" : Encoding.UTF8.GetString(frame.Body.Span));

                        await _handler.OnEventAsync(this, ctx, cancellationToken)
                            .ConfigureAwait(false);
                    }
                }

                // ── Check for auth timeout even when no frame just arrived ──
                var timeoutCheck = await _connection.CheckTimeoutsAsync(cancellationToken)
                    .ConfigureAwait(false);

                if (timeoutCheck.ShouldClose)
                {
                    foreach (var outbound in timeoutCheck.OutboundFrames)
                    {
                        await SendFrameAsync(outbound, cancellationToken).ConfigureAwait(false);
                    }

                    closeCode = timeoutCheck.CloseCode;
                    break;
                }

                // ── Session closed by Core (e.g. protocol error) ───────────
                if (result.ShouldClose)
                {
                    closeCode = result.CloseCode;
                    break;
                }
            }
        }
        catch (WebSocketException ex)
        {
            _logger.LogDebug("WebSocket exception for session {SessionId}: {Message}",
                _connection.SessionId, ex.Message);
        }
        catch (OperationCanceledException)
        {
            _logger.LogDebug("Session {SessionId} cancelled.", _connection.SessionId);
        }
        catch (Exception ex)
        {
            _logger.LogError(ex, "Unhandled error in session {SessionId}.", _connection.SessionId);
        }
        finally
        {
            _closed = true;

            // Mark session as disconnected in Core (saves resumable state)
            await _connection.DisconnectAsync(CancellationToken.None).ConfigureAwait(false);

            // Notify handler
            if (handlerConnected)
            {
                try
                {
                    await _handler.OnDisconnectedAsync(this, closeCode, CancellationToken.None)
                        .ConfigureAwait(false);
                }
                catch (Exception ex)
                {
                    _logger.LogError(ex, "Error in OnDisconnectedAsync for session {SessionId}.",
                        _connection.SessionId);
                }
            }

            // Close WebSocket transport if still open
            try
            {
                if (_ws.State == WebSocketState.Open || _ws.State == WebSocketState.CloseReceived)
                {
                    await _ws.CloseAsync(
                        WebSocketCloseStatus.NormalClosure,
                        "CulpeoStream session ended",
                        CancellationToken.None).ConfigureAwait(false);
                }
            }
            catch (Exception ex)
            {
                _logger.LogDebug("WebSocket close failed for session {SessionId}: {Message}",
                    _connection.SessionId, ex.Message);
            }

            _sendLock.Dispose();
        }
    }

    // ── Private helpers ───────────────────────────────────────────────────────

    private async Task<(WebSocketMessageType type, byte[] data)> ReceiveMessageAsync(
        CancellationToken cancellationToken)
    {
        // Reassemble potentially fragmented WebSocket messages (RFC 6455 §5.4).
        // Use ArraySegment overload which returns WebSocketReceiveResult (not ValueWebSocketReceiveResult).
        var buffer = new byte[4096];
        using var accumulator = new System.IO.MemoryStream();

        WebSocketReceiveResult result;
        do
        {
            result = await _ws.ReceiveAsync(new ArraySegment<byte>(buffer), cancellationToken)
                .ConfigureAwait(false);

            if (result.MessageType == WebSocketMessageType.Close)
            {
                return (WebSocketMessageType.Close, []);
            }

            accumulator.Write(buffer, 0, result.Count);
        } while (!result.EndOfMessage);

        return (result.MessageType, accumulator.ToArray());
    }

    private async Task SendFrameAsync(CulpeoFrame frame, CancellationToken cancellationToken)
    {
        var bytes = await _serializer.SerializeAsync(frame, cancellationToken)
            .ConfigureAwait(false);

        var messageType = frame.Kind == CulpeoFrameKind.Control
            ? WebSocketMessageType.Text
            : WebSocketMessageType.Binary;

        await _sendLock.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (_ws.State != WebSocketState.Open)
            {
                return;
            }

            await _ws.SendAsync(bytes.AsMemory(), messageType, endOfMessage: true, cancellationToken)
                .ConfigureAwait(false);
        }
        finally
        {
            _sendLock.Release();
        }
    }

    private static CulpeoFrame BuildCloseFrame(string code, string reason)
        => new(
            CulpeoFrameKind.Control,
            "{}"u8.ToArray(),
            @event: "culpeo.close",
            contentType: "application/json",
            code: code,
            reason: reason);

    private long ComputeTimestampMicros()
    {
        var elapsed = DateTimeOffset.UtcNow - _sessionStart;
        return (long)(elapsed.TotalMilliseconds * 1000.0);
    }

    /// <summary>
    /// Returns <see langword="true"/> for well-formed, non-<c>culpeo.</c> events.
    /// Protocol events are handled by Core; only application events reach the handler.
    /// </summary>
    private static bool IsApplicationEvent(string? eventName)
        => !string.IsNullOrEmpty(eventName)
           && !eventName.StartsWith("culpeo.", StringComparison.Ordinal);
}
