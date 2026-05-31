using System.Globalization;
using System.Net.WebSockets;
using System.Runtime.CompilerServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Threading.Channels;
using CulpeoStream.Core;

namespace CulpeoStream.Client;

/// <summary>
/// CulpeoStream client. Connects to a server, performs the handshake, sends media,
/// and exposes received frames as an <see cref="IAsyncEnumerable{T}"/>.
///
/// <para>
/// Thread-safety: <see cref="SendMediaAsync"/> and <see cref="SendEventAsync"/> may
/// be called concurrently. <see cref="ReceiveAsync"/> must be consumed by a single reader.
/// </para>
/// </summary>
public sealed class CulpeoStreamClient : IAsyncDisposable
{
    // ── Protocol infrastructure (stateless, thread-safe) ─────────────────────

    private readonly CulpeoMessageParser _parser = new();
    private readonly CulpeoMessageSerializer _serializer = new();

    // ── Options ───────────────────────────────────────────────────────────────

    private readonly CulpeoStreamClientOptions _options;

    // ── Event channel: bridges receive loop → ReceiveAsync ───────────────────
    // Bounded with back-pressure (Wait mode). Callers MUST consume ReceiveAsync
    // continuously to avoid blocking the receive loop. See CS-P3-002.

    private readonly Channel<CulpeoClientEvent> _eventChannel;

    // ── Send serialization ────────────────────────────────────────────────────

    private readonly SemaphoreSlim _sendLock = new(1, 1);

    // ── Connect guard (CS-P3-001) ─────────────────────────────────────────────
    // Prevents two concurrent ConnectAsync callers from both passing the _loopTask null-check
    // and both starting a reconnect loop. 0 = idle, 1 = connecting/connected.
    // CAS is sufficient here — no async wait needed; second caller should fail fast, not queue.

    private int _connectState = 0;

    // ── Session state (read/written only from ConnectAsync and the loop task) ─

    private string? _sessionId;
    /// <summary>Server-assigned stream info indexed by server-assigned stream ID.</summary>
    private Dictionary<string, ClientStreamState> _streamStates = new(StringComparer.Ordinal);

    // ── Connection ────────────────────────────────────────────────────────────

    private WebSocket? _ws;
    private Uri? _serverUri;

    // cancelled by DisposeAsync to tear everything down
    private readonly CancellationTokenSource _lifetimeCts = new();

    // the background loop task (started by ConnectAsync)
    private Task? _loopTask;

    // ── State machine ─────────────────────────────────────────────────────────

    private volatile CulpeoClientState _state = CulpeoClientState.Disconnected;

    // ── WebSocket factory (overrideable in tests) ─────────────────────────────

    /// <summary>
    /// Internal: injectable factory for WebSocket creation. Allows test code to
    /// supply a pre-connected <see cref="WebSocket"/> without a real network.
    /// </summary>
    internal Func<Uri, CancellationToken, Task<WebSocket>>? WebSocketFactory { get; set; }

    // ── Public API ────────────────────────────────────────────────────────────

    public CulpeoStreamClient(CulpeoStreamClientOptions options)
    {
        _options = options ?? throw new ArgumentNullException(nameof(options));
        if (options.Streams is null || options.Streams.Count == 0)
        {
            throw new ArgumentException("At least one stream must be declared.", nameof(options));
        }

        // SEC-018: require at least one auth source
        if (options.Authorization is null && options.GetToken is null)
        {
            throw new ArgumentException(
                "At least one of Authorization or GetToken must be non-null.", nameof(options));
        }

        // CS-P3-002: bounded channel with configurable capacity
        _eventChannel = Channel.CreateBounded<CulpeoClientEvent>(
            new BoundedChannelOptions(options.EventChannelCapacity)
            {
                FullMode = BoundedChannelFullMode.Wait,
                SingleReader = false,
                SingleWriter = true
            });
    }

    /// <summary>Current connection state.</summary>
    public CulpeoClientState State => _state;

    /// <summary>
    /// Server-assigned stream info confirmed in the most recent <c>culpeo.init-ack</c>.
    /// Empty before connection is established.
    /// </summary>
    public IReadOnlyList<CulpeoStreamInfo> Streams =>
        _streamStates.Values
            .Select(s => new CulpeoStreamInfo(s.ServerId, s.ContentType, s.Type, s.Purpose, s.SendOffset, s.OffsetType))
            .ToList();

    /// <summary>
    /// Connects to the server, performs the <c>culpeo.init</c> handshake, and starts
    /// the background receive loop. Throws on handshake failure.
    /// </summary>
    public async Task ConnectAsync(Uri serverUri, CancellationToken cancellationToken = default)
    {
        // CS-P3-001: atomic guard — second caller fails immediately, no queuing.
        // Reset to 0 on failure so the caller can retry with a new attempt.
        if (Interlocked.CompareExchange(ref _connectState, 1, 0) != 0)
            throw new InvalidOperationException("ConnectAsync has already been called.");
        try
        {
            // SEC: enforce wss:// by default (§3.1)
            if (!_options.AllowInsecureConnections
                && string.Equals(serverUri.Scheme, "ws", StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidOperationException(
                    "Insecure WebSocket connections (ws://) are not allowed by default. " +
                    "Set AllowInsecureConnections = true in CulpeoStreamClientOptions to allow " +
                    "this in local development environments only.");
            }

            _serverUri = serverUri;
            _state = CulpeoClientState.Connecting;

            using var linkedCts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, _lifetimeCts.Token);

            // ── First connect (synchronous from caller's perspective) ─────────────
            _ws = await CreateWebSocketAsync(serverUri, linkedCts.Token).ConfigureAwait(false);

            bool isResumption = false;
            await PerformHandshakeAsync(_ws, isResumption, linkedCts.Token).ConfigureAwait(false);

            _state = CulpeoClientState.Established;

            // ── Emit SessionEstablished before starting the loop ──────────────────
            await _eventChannel.Writer.WriteAsync(new SessionEstablished(_sessionId!), linkedCts.Token)
                .ConfigureAwait(false);

            // ── Start background loop (using lifetime token, not caller's) ────────
            _loopTask = Task.Run(() => RunReceiveLoopAsync(_lifetimeCts.Token), CancellationToken.None);
        }
        catch
        {
            Interlocked.Exchange(ref _connectState, 0); // allow retry on failure
            throw;
        }
    }

    /// <summary>
    /// Sends a graceful <c>culpeo.close</c> and disconnects.
    /// </summary>
    public async Task DisconnectAsync(CancellationToken cancellationToken = default)
    {
        if (_state == CulpeoClientState.Disconnected)
        {
            return;
        }

        // Send culpeo.close if connected
        if (_ws is { State: WebSocketState.Open })
        {
            try
            {
                var closeFrame = BuildCloseFrame("normal", "Client disconnecting.");
                await SendFrameAsync(closeFrame, cancellationToken).ConfigureAwait(false);
            }
            catch
            {
                // Best-effort; ignore errors during shutdown
            }
        }

        _lifetimeCts.Cancel();
        _state = CulpeoClientState.Disconnected;

        if (_loopTask is not null)
        {
            try { await _loopTask.ConfigureAwait(false); }
            catch { /* suppress */ }
        }
    }

    /// <summary>
    /// Sends a media frame on the specified stream.
    /// The stream offset is tracked and incremented automatically.
    /// </summary>
    /// <param name="streamId">Server-assigned stream identifier (from <see cref="Streams"/>).</param>
    /// <param name="data">Raw media payload.</param>
    /// <exception cref="InvalidOperationException">Session is not established.</exception>
    /// <exception cref="KeyNotFoundException">Stream ID is not known.</exception>
    public async Task SendMediaAsync(string streamId, ReadOnlyMemory<byte> data, CancellationToken cancellationToken = default)
    {
        if (_state != CulpeoClientState.Established)
        {
            throw new InvalidOperationException($"Cannot send media in state {_state}.");
        }

        if (!_streamStates.TryGetValue(streamId, out var stream))
        {
            throw new KeyNotFoundException($"Stream '{streamId}' is not registered.");
        }

        if (stream.Type == CulpeoStreamType.Output)
        {
            throw new InvalidOperationException(
                $"Stream '{streamId}' is an output stream; client cannot send media on it.");
        }

        // CS-P3-004: read offset, serialize, send, and advance offset all inside _sendLock
        // so that concurrent callers cannot interleave their offset reads and updates.
        // We serialize with the real offset (read inside the lock) to ensure the frame
        // carries the correct, contiguous position.
        await _sendLock.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var offset = stream.SendOffset;

            var frame = new CulpeoMessage(
                CulpeoMessageKind.Media,
                data,
                contentType: stream.ContentType,
                streamId: streamId,
                offset: offset);

            var frameBytes = await _serializer.SerializeAsync(frame, cancellationToken).ConfigureAwait(false);

            if (_ws is { State: WebSocketState.Open })
            {
                await _ws.SendAsync(frameBytes.AsMemory(), WebSocketMessageType.Binary,
                    endOfMessage: true, cancellationToken).ConfigureAwait(false);
            }

            // Advance offset only after successful send
            stream.SendOffset = offset + ComputeOffsetIncrement(stream, data.Length);
        }
        finally
        {
            _sendLock.Release();
        }
    }

    /// <summary>
    /// Sends an application event frame.
    /// </summary>
    /// <param name="eventName">Namespaced event name (must not use <c>culpeo.</c> prefix).</param>
    /// <param name="body">Optional body to serialize as JSON. <see langword="null"/> emits <c>{}</c>.</param>
    public async Task SendEventAsync(string eventName, object? body, CancellationToken cancellationToken = default)
    {
        if (_state != CulpeoClientState.Established)
        {
            throw new InvalidOperationException($"Cannot send event in state {_state}.");
        }

        if (string.IsNullOrEmpty(eventName))
        {
            throw new ArgumentException("Event name must not be null or empty.", nameof(eventName));
        }

        if (eventName.StartsWith("culpeo.", StringComparison.Ordinal))
        {
            throw new ArgumentException(
                "Application events must not use the reserved 'culpeo.' namespace.", nameof(eventName));
        }

        byte[] bodyBytes = body is null
            ? "{}"u8.ToArray()
            : Encoding.UTF8.GetBytes(JsonSerializer.Serialize(body));

        var frame = new CulpeoMessage(
            CulpeoMessageKind.Control,
            bodyBytes,
            @event: eventName,
            contentType: "application/json");

        await SendFrameAsync(frame, cancellationToken).ConfigureAwait(false);
    }

    /// <summary>
    /// Yields incoming events as an async stream. Completes when the session ends.
    /// Must be consumed by a single reader.
    /// </summary>
    public async IAsyncEnumerable<CulpeoClientEvent> ReceiveAsync(
        [EnumeratorCancellation] CancellationToken cancellationToken = default)
    {
        await foreach (var evt in _eventChannel.Reader.ReadAllAsync(cancellationToken).ConfigureAwait(false))
        {
            yield return evt;
        }
    }

    // ── IAsyncDisposable ──────────────────────────────────────────────────────

    public async ValueTask DisposeAsync()
    {
        // CS-P3-003: cancel first, then await the loop to full completion before touching _ws.
        // The loop may create a new _ws on reconnect; awaiting it guarantees no new WebSocket
        // can be created after we dispose.
        _lifetimeCts.Cancel();

        if (_loopTask is not null)
        {
            try { await _loopTask.ConfigureAwait(false); }
            catch { /* suppress */ }
        }

        // Loop has stopped — it is now safe to dispose the current WebSocket
        _ws?.Dispose();
        _sendLock.Dispose();
        _lifetimeCts.Dispose();
        _eventChannel.Writer.TryComplete();
    }

    // ── Background receive loop ───────────────────────────────────────────────

    private async Task RunReceiveLoopAsync(CancellationToken cancellationToken)
    {
        try
        {
            await ReceiveFramesAsync(cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            // Disposed or DisconnectAsync called — clean exit
        }
        catch (Exception)
        {
            // Connection dropped unexpectedly
        }
        finally
        {
            if (!cancellationToken.IsCancellationRequested && _options.AutoReconnect)
            {
                await ReconnectLoopAsync(cancellationToken).ConfigureAwait(false);
            }
            else
            {
                _state = CulpeoClientState.Disconnected;
                _eventChannel.Writer.TryComplete();
            }
        }
    }

    /// <summary>
    /// Inner receive loop: processes frames until the connection closes or throws.
    /// </summary>
    private async Task ReceiveFramesAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            WebSocketMessageType messageType;
            byte[] messageBytes;

            try
            {
                (messageType, messageBytes) = await ReceiveMessageAsync(_ws!, cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (WebSocketException)
            {
                return; // connection dropped
            }
            catch (OperationCanceledException)
            {
                throw; // propagate cancellation
            }

            if (messageType == WebSocketMessageType.Close)
            {
                return;
            }

            CulpeoMessage frame;
            try
            {
                var kind = messageType == WebSocketMessageType.Text
                    ? CulpeoMessageKind.Control
                    : CulpeoMessageKind.Media;

                frame = await _parser.ParseAsync(messageBytes, kind, cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (FormatException)
            {
                // Malformed frame — close with protocol error
                var errClose = BuildCloseFrame("protocol-error", "Malformed frame.");
                await TrySendFrameAsync(errClose).ConfigureAwait(false);
                return;
            }

            bool shouldStop = await DispatchFrameAsync(frame, cancellationToken).ConfigureAwait(false);
            if (shouldStop)
            {
                return;
            }
        }
    }

    /// <summary>
    /// Dispatches a received frame to the appropriate handler.
    /// Returns <see langword="true"/> if the receive loop should stop.
    /// </summary>
    private async Task<bool> DispatchFrameAsync(CulpeoMessage frame, CancellationToken cancellationToken)
    {
        if (frame.Kind == CulpeoMessageKind.Media)
        {
            return await HandleIncomingMediaAsync(frame, cancellationToken).ConfigureAwait(false);
        }

        return frame.Event switch
        {
            "culpeo.close" => await HandleCloseAsync(frame, cancellationToken).ConfigureAwait(false),
            "culpeo.auth-refresh" => await HandleAuthRefreshAsync(frame, cancellationToken).ConfigureAwait(false),
            "culpeo.ping" => await HandlePingAsync(frame, cancellationToken).ConfigureAwait(false),
            "culpeo.pong" => false, // no-op for client
            _ => await HandleApplicationEventAsync(frame, cancellationToken).ConfigureAwait(false)
        };
    }

    private async Task<bool> HandleIncomingMediaAsync(CulpeoMessage frame, CancellationToken cancellationToken)
    {
        var streamId = frame.StreamId;
        if (string.IsNullOrEmpty(streamId) || !_streamStates.TryGetValue(streamId, out var stream))
        {
            // Unknown stream — protocol error
            var errClose = BuildCloseFrame("protocol-error", "Media frame references unknown stream.");
            await TrySendFrameAsync(errClose).ConfigureAwait(false);
            return true;
        }

        // SEC: Validate Content-Type against declared stream (§6.2)
        if (!string.IsNullOrWhiteSpace(frame.ContentType)
            && !ContentTypeMatchesStream(frame.ContentType, stream.ContentType))
        {
            var errClose = BuildCloseFrame("protocol-error",
                $"Media frame Content-Type '{frame.ContentType}' does not match declared '{stream.ContentType}'.");
            await TrySendFrameAsync(errClose).ConfigureAwait(false);
            return true;
        }

        var offset = frame.Offset ?? 0;

        // Track highest received offset for session resumption (§8.2, §7.2)
        if (offset >= stream.ReceiveOffset)
        {
            stream.ReceiveOffset = offset + ComputeOffsetIncrement(stream, frame.Body.Length);
        }

        await _eventChannel.Writer.WriteAsync(
            new MediaReceived(streamId, frame.Body, offset), cancellationToken)
            .ConfigureAwait(false);

        return false;
    }

    private async Task<bool> HandleCloseAsync(CulpeoMessage frame, CancellationToken cancellationToken)
    {
        // Echo close back per spec §6.1 (culpeo.close)
        var responseClose = BuildCloseFrame(frame.Code ?? "normal", frame.Reason ?? "Session closed");
        await TrySendFrameAsync(responseClose).ConfigureAwait(false);

        _state = CulpeoClientState.Disconnected;
        var reason = frame.Code ?? "normal";
        await _eventChannel.Writer.WriteAsync(new Disconnected(reason), cancellationToken)
            .ConfigureAwait(false);
        _eventChannel.Writer.TryComplete();
        return true;
    }

    private async Task<bool> HandleAuthRefreshAsync(CulpeoMessage frame, CancellationToken cancellationToken)
    {
        // Parse nonce
        string? nonce = null;
        try
        {
            using var doc = JsonDocument.Parse(frame.Body.IsEmpty ? "{}"u8.ToArray() : frame.Body.ToArray());
            if (doc.RootElement.TryGetProperty("nonce", out var nonceProp))
            {
                nonce = nonceProp.GetString();
            }
        }
        catch (JsonException) { /* ignore; we'll close below */ }

        if (string.IsNullOrEmpty(nonce))
        {
            var errClose = BuildCloseFrame("protocol-error", "culpeo.auth-refresh missing nonce.");
            await TrySendFrameAsync(errClose).ConfigureAwait(false);
            return true;
        }

        // If no GetToken callback, close with auth-error
        if (_options.GetToken is null)
        {
            var errClose = BuildCloseFrame("auth-expired", "No token refresh callback configured.");
            await TrySendFrameAsync(errClose).ConfigureAwait(false);
            return true;
        }

        string newToken;
        try
        {
            newToken = await _options.GetToken(cancellationToken).ConfigureAwait(false);
        }
        catch (Exception)
        {
            var errClose = BuildCloseFrame("auth-expired", "Token refresh failed.");
            await TrySendFrameAsync(errClose).ConfigureAwait(false);
            return true;
        }

        if (string.IsNullOrEmpty(newToken))
        {
            var errClose = BuildCloseFrame("auth-expired", "Token refresh returned empty token.");
            await TrySendFrameAsync(errClose).ConfigureAwait(false);
            return true;
        }

        // Send culpeo.auth-response echoing the nonce (§6.1)
        var nonceBodyBytes = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(new { nonce }));
        var authResponse = new CulpeoMessage(
            CulpeoMessageKind.Control,
            nonceBodyBytes,
            @event: "culpeo.auth-response",
            authorization: newToken,
            contentType: "application/json");

        await SendFrameAsync(authResponse, cancellationToken).ConfigureAwait(false);
        return false;
    }

    private async Task<bool> HandlePingAsync(CulpeoMessage frame, CancellationToken cancellationToken)
    {
        // Respond with pong echoing ts (§6.1)
        long ts = 0;
        try
        {
            using var doc = JsonDocument.Parse(frame.Body.IsEmpty ? "{}"u8.ToArray() : frame.Body.ToArray());
            if (doc.RootElement.TryGetProperty("ts", out var tsProp))
            {
                ts = tsProp.GetInt64();
            }
        }
        catch (JsonException) { /* use ts=0 */ }

        var serverTs = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() * 1000;
        var pongBodyBytes = Encoding.UTF8.GetBytes(
            JsonSerializer.Serialize(new { ts, server_ts = serverTs }));

        var pong = new CulpeoMessage(
            CulpeoMessageKind.Control,
            pongBodyBytes,
            @event: "culpeo.pong",
            contentType: "application/json");

        await TrySendFrameAsync(pong).ConfigureAwait(false);
        return false;
    }

    private async Task<bool> HandleApplicationEventAsync(CulpeoMessage frame, CancellationToken cancellationToken)
    {
        // Unknown protocol events or malformed culpeo.* events: treat as protocol error
        if (!string.IsNullOrEmpty(frame.Event) && frame.Event.StartsWith("culpeo.", StringComparison.Ordinal))
        {
            // Unknown culpeo. event — ignore for forward-compatibility (§10.2)
            return false;
        }

        // Application event (§9.1): forward to caller
        if (!string.IsNullOrEmpty(frame.Event))
        {
            JsonElement body = default;
            try
            {
                using var doc = JsonDocument.Parse(frame.Body.IsEmpty ? "{}"u8.ToArray() : frame.Body.ToArray());
                body = doc.RootElement.Clone();
            }
            catch (JsonException) { /* use default JsonElement */ }

            await _eventChannel.Writer.WriteAsync(
                new ApplicationEventReceived(frame.Event, body), cancellationToken)
                .ConfigureAwait(false);
        }

        return false;
    }

    // ── Reconnection loop ─────────────────────────────────────────────────────

    private async Task ReconnectLoopAsync(CancellationToken cancellationToken)
    {
        _state = CulpeoClientState.Reconnecting;

        for (int attempt = 0; attempt < _options.MaxReconnectAttempts; attempt++)
        {
            if (cancellationToken.IsCancellationRequested)
            {
                break;
            }

            // Full-jitter exponential backoff: random(0, min(MaxBackoff, InitialBackoff * 2^attempt))
            var cap = TimeSpan.FromSeconds(Math.Min(
                _options.MaxBackoff.TotalSeconds,
                _options.InitialBackoff.TotalSeconds * Math.Pow(2, attempt)));

            var jitterMs = (int)(RandomNumberGenerator.GetInt32(0, (int)(cap.TotalMilliseconds + 1)));
            var delay = TimeSpan.FromMilliseconds(jitterMs);

            try
            {
                await Task.Delay(delay, cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                break;
            }

            try
            {
                _ws?.Dispose();
                _ws = await CreateWebSocketAsync(_serverUri!, cancellationToken).ConfigureAwait(false);

                bool isResumption = _sessionId is not null;
                await PerformHandshakeAsync(_ws, isResumption, cancellationToken).ConfigureAwait(false);

                _state = CulpeoClientState.Established;

                await _eventChannel.Writer.WriteAsync(
                    new SessionResumed(_sessionId!), cancellationToken)
                    .ConfigureAwait(false);

                // Successfully reconnected — re-enter receive loop
                await ReceiveFramesAsync(cancellationToken).ConfigureAwait(false);

                // If we get here, the connection dropped again — loop to retry
                _state = CulpeoClientState.Reconnecting;
            }
            catch (InvalidOperationException ex) when (ex.Message.Contains("invalid-session"))
            {
                // Server rejected our session ID — start fresh
                _sessionId = null;
                foreach (var stream in _streamStates.Values)
                {
                    stream.SendOffset = 0;
                    stream.ReceiveOffset = 0;
                    stream.ServerId = stream.ServerId; // keep structure
                }
            }
            catch (CulpeoProtocolException)
            {
                // SEC-019: non-resumable protocol error (e.g. invalid resume_offset from server)
                // — clear stored session and offsets so next attempt is a fresh connect.
                _sessionId = null;
                foreach (var stream in _streamStates.Values)
                {
                    stream.SendOffset = 0;
                    stream.ReceiveOffset = 0;
                }
            }
            catch (OperationCanceledException)
            {
                break;
            }
            catch (Exception)
            {
                // Connection failed; continue retry loop
            }
        }

        // All attempts exhausted or cancelled
        _state = CulpeoClientState.Disconnected;
        await _eventChannel.Writer.WriteAsync(
            new Disconnected("Max reconnection attempts reached."), CancellationToken.None)
            .ConfigureAwait(false);
        _eventChannel.Writer.TryComplete();
    }

    // ── Handshake ─────────────────────────────────────────────────────────────

    /// <summary>
    /// Returns the current bearer token, preferring <see cref="CulpeoStreamClientOptions.GetToken"/>
    /// over the static <see cref="CulpeoStreamClientOptions.Authorization"/> field so that
    /// reconnects always use a fresh token rather than a potentially-expired static one (SEC-018).
    /// </summary>
    private async Task<string> GetCurrentTokenAsync(CancellationToken ct)
    {
        if (_options.GetToken is not null)
            return await _options.GetToken(ct).ConfigureAwait(false);
        return _options.Authorization ?? string.Empty;
    }

    /// <summary>
    /// Performs the <c>culpeo.init</c> → <c>culpeo.init-ack</c> handshake.
    /// On success updates <see cref="_sessionId"/> and <see cref="_streamStates"/>.
    /// </summary>
    private async Task PerformHandshakeAsync(WebSocket ws, bool isResumption, CancellationToken cancellationToken)
    {
        // SEC-018: always fetch a fresh token via GetCurrentTokenAsync so reconnects do not
        // reuse a static (potentially-expired) Authorization string.
        var token = await GetCurrentTokenAsync(cancellationToken).ConfigureAwait(false);

        var initBody = BuildInitBody(isResumption);
        var initFrame = new CulpeoMessage(
            CulpeoMessageKind.Control,
            Encoding.UTF8.GetBytes(initBody),
            @event: "culpeo.init",
            authorization: token,
            contentType: "application/json",
            sessionId: isResumption ? _sessionId : null,
            bufferWindow: _options.BufferWindowMs);

        var initBytes = await _serializer.SerializeAsync(initFrame, cancellationToken).ConfigureAwait(false);
        await ws.SendAsync(initBytes.AsMemory(), WebSocketMessageType.Text, endOfMessage: true, cancellationToken)
            .ConfigureAwait(false);

        // Receive init-ack or init-error
        var (msgType, msgBytes) = await ReceiveMessageAsync(ws, cancellationToken).ConfigureAwait(false);
        if (msgType != WebSocketMessageType.Text)
        {
            throw new InvalidOperationException("Expected text frame in response to culpeo.init.");
        }

        var response = await _parser.ParseAsync(msgBytes, CulpeoMessageKind.Control, cancellationToken)
            .ConfigureAwait(false);

        if (string.Equals(response.Event, "culpeo.init-error", StringComparison.Ordinal))
        {
            var code = response.Code ?? "server-error";
            // Signal to reconnect loop to clear session on invalid-session
            throw new InvalidOperationException($"culpeo.init-error [{code}]: {response.Reason ?? "(no reason)"}");
        }

        if (!string.Equals(response.Event, "culpeo.init-ack", StringComparison.Ordinal))
        {
            throw new InvalidOperationException(
                $"Unexpected event '{response.Event}' during handshake.");
        }

        // Parse init-ack body
        ProcessInitAck(response, isResumption);
    }

    /// <summary>
    /// Parses <c>culpeo.init-ack</c> and updates <see cref="_sessionId"/> and
    /// <see cref="_streamStates"/>.
    /// </summary>
    private void ProcessInitAck(CulpeoMessage ackFrame, bool isResumption)
    {
        _sessionId = ackFrame.SessionId
            ?? throw new InvalidOperationException("culpeo.init-ack missing Session-Id.");

        if (ackFrame.Body.IsEmpty)
        {
            throw new InvalidOperationException("culpeo.init-ack missing body.");
        }

        using var doc = JsonDocument.Parse(ackFrame.Body.ToArray());
        var root = doc.RootElement;

        if (!root.TryGetProperty("streams", out var streamsEl)
            || streamsEl.ValueKind != JsonValueKind.Array)
        {
            throw new InvalidOperationException("culpeo.init-ack body missing 'streams' array.");
        }

        var newStates = new Dictionary<string, ClientStreamState>(StringComparer.Ordinal);

        foreach (var streamEl in streamsEl.EnumerateArray())
        {
            var serverId = streamEl.TryGetProperty("id", out var idProp) ? idProp.GetString() : null;
            var contentType = streamEl.TryGetProperty("content_type", out var ctProp) ? ctProp.GetString() : null;
            var typeStr = streamEl.TryGetProperty("type", out var typeProp) ? typeProp.GetString() : null;
            var offsetTypeStr = streamEl.TryGetProperty("offset_type", out var otProp) ? otProp.GetString() : null;
            var purpose = streamEl.TryGetProperty("purpose", out var purposeProp) ? purposeProp.GetString() : null;

            if (string.IsNullOrEmpty(serverId) || string.IsNullOrEmpty(contentType)
                || !TryParseStreamType(typeStr, out var streamType)
                || !TryParseOffsetType(offsetTypeStr, out var offsetType))
            {
                throw new InvalidOperationException("culpeo.init-ack stream entry is missing required fields.");
            }

            // Find the matching declaration to preserve tracked offsets across reconnections
            ClientStreamState? existing = isResumption
                ? FindMatchingStream(streamType, contentType, purpose)
                : null;

            var state = new ClientStreamState
            {
                ServerId = serverId,
                ContentType = contentType,
                Type = streamType,
                OffsetType = offsetType,
                Purpose = purpose,
                PcmBytesPerSample = ComputePcmBytesPerSample(contentType, offsetType),
                SendOffset = existing?.SendOffset ?? 0,
                ReceiveOffset = existing?.ReceiveOffset ?? 0,
            };

            // On resumption, server may confirm a different offset — use server's value
            if (isResumption && streamEl.TryGetProperty("resume_offset", out var roProp))
            {
                var confirmedOffset = roProp.GetInt64();

                // SEC-019: validate server-supplied offset; a malicious or buggy server must
                // not be able to push our cursor to a negative or future position.
                if (confirmedOffset < 0)
                {
                    throw new CulpeoProtocolException("protocol-error",
                        $"Server sent negative resume_offset ({confirmedOffset}) for stream '{serverId}'.");
                }

                // For send streams: the confirmed offset must not exceed what we have sent.
                if ((streamType is CulpeoStreamType.Input or CulpeoStreamType.Duplex)
                    && existing is not null
                    && confirmedOffset > existing.SendOffset)
                {
                    throw new CulpeoProtocolException("protocol-error",
                        $"Server resume_offset {confirmedOffset} exceeds client tracked send offset {existing.SendOffset} for stream '{serverId}'.");
                }

                // For send streams: server's confirmed receive offset is our new send cursor
                if (streamType is CulpeoStreamType.Input or CulpeoStreamType.Duplex)
                {
                    state.SendOffset = confirmedOffset;
                }
                // For receive streams: server resumes from confirmedOffset
                if (streamType is CulpeoStreamType.Output or CulpeoStreamType.Duplex)
                {
                    state.ReceiveOffset = confirmedOffset;
                }
            }

            newStates[serverId] = state;
        }

        _streamStates = newStates;
    }

    /// <summary>
    /// Finds a <see cref="ClientStreamState"/> in the existing map that matches the given
    /// type/content-type/purpose (used when server reassigns stream IDs on reconnect).
    /// </summary>
    private ClientStreamState? FindMatchingStream(CulpeoStreamType type, string contentType, string? purpose)
    {
        return _streamStates.Values.FirstOrDefault(s =>
            s.Type == type
            && string.Equals(s.ContentType, contentType, StringComparison.OrdinalIgnoreCase)
            && string.Equals(s.Purpose, purpose, StringComparison.Ordinal));
    }

    // ── Init body builder ─────────────────────────────────────────────────────

    private string BuildInitBody(bool isResumption)
    {
        using var ms = new System.IO.MemoryStream();
        using var writer = new Utf8JsonWriter(ms);

        writer.WriteStartObject();
        writer.WriteString("version", _options.Version);
        writer.WriteStartArray("streams");

        foreach (var decl in _options.Streams)
        {
            writer.WriteStartObject();
            writer.WriteString("content_type", decl.ContentType);
            writer.WriteString("type", StreamTypeToString(decl.Type));
            writer.WriteString("offset_type", OffsetTypeToString(decl.OffsetType));
            if (!string.IsNullOrEmpty(decl.Purpose))
            {
                writer.WriteString("purpose", decl.Purpose);
            }

            if (isResumption)
            {
                // Include previously assigned stream ID as a hint
                var existing = _streamStates.Values.FirstOrDefault(s =>
                    s.Type == decl.Type
                    && string.Equals(s.ContentType, decl.ContentType, StringComparison.OrdinalIgnoreCase)
                    && string.Equals(s.Purpose, decl.Purpose, StringComparison.Ordinal));

                if (existing is not null)
                {
                    writer.WriteString("id", existing.ServerId);

                    // resume_offset: for send streams use send offset; for receive streams use receive offset
                    long resumeOffset = decl.Type switch
                    {
                        CulpeoStreamType.Input => existing.SendOffset,
                        CulpeoStreamType.Output => existing.ReceiveOffset,
                        CulpeoStreamType.Duplex => existing.SendOffset,
                        _ => 0
                    };

                    writer.WriteNumber("resume_offset", resumeOffset);
                }
            }

            writer.WriteEndObject();
        }

        writer.WriteEndArray();
        writer.WriteEndObject();
        writer.Flush();

        return Encoding.UTF8.GetString(ms.ToArray());
    }

    // ── Transport helpers ─────────────────────────────────────────────────────

    private async Task<WebSocket> CreateWebSocketAsync(Uri uri, CancellationToken cancellationToken)
    {
        if (WebSocketFactory is not null)
        {
            return await WebSocketFactory(uri, cancellationToken).ConfigureAwait(false);
        }

        var ws = new ClientWebSocket();
        ws.Options.AddSubProtocol("culpeostream");
        await ws.ConnectAsync(uri, cancellationToken).ConfigureAwait(false);
        return ws;
    }

    private async Task<(WebSocketMessageType type, byte[] data)> ReceiveMessageAsync(
        WebSocket ws, CancellationToken cancellationToken)
    {
        var buffer = new byte[4096];
        using var accumulator = new System.IO.MemoryStream();

        WebSocketReceiveResult result;
        do
        {
            result = await ws.ReceiveAsync(new ArraySegment<byte>(buffer), cancellationToken)
                .ConfigureAwait(false);

            if (result.MessageType == WebSocketMessageType.Close)
            {
                return (WebSocketMessageType.Close, []);
            }

            accumulator.Write(buffer, 0, result.Count);
        } while (!result.EndOfMessage);

        return (result.MessageType, accumulator.ToArray());
    }

    private async Task SendFrameAsync(CulpeoMessage frame, CancellationToken cancellationToken)
    {
        var bytes = await _serializer.SerializeAsync(frame, cancellationToken).ConfigureAwait(false);
        var messageType = frame.Kind == CulpeoMessageKind.Control
            ? WebSocketMessageType.Text
            : WebSocketMessageType.Binary;

        await _sendLock.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (_ws is { State: WebSocketState.Open })
            {
                await _ws.SendAsync(bytes.AsMemory(), messageType, endOfMessage: true, cancellationToken)
                    .ConfigureAwait(false);
            }
        }
        finally
        {
            _sendLock.Release();
        }
    }

    /// <summary>Best-effort send that suppresses exceptions (used during cleanup).</summary>
    private async Task TrySendFrameAsync(CulpeoMessage frame)
    {
        try { await SendFrameAsync(frame, CancellationToken.None).ConfigureAwait(false); }
        catch { /* best effort */ }
    }

    private static CulpeoMessage BuildCloseFrame(string code, string reason)
        => new(
            CulpeoMessageKind.Control,
            "{}"u8.ToArray(),
            @event: "culpeo.close",
            contentType: "application/json",
            code: code,
            reason: reason);

    // ── Offset calculation ────────────────────────────────────────────────────

    private static long ComputeOffsetIncrement(ClientStreamState stream, int payloadLength)
        => stream.OffsetType switch
        {
            OffsetType.Message => 1,
            OffsetType.Byte => payloadLength,
            OffsetType.Time => stream.PcmBytesPerSample.HasValue && stream.PcmBytesPerSample.Value > 0
                ? payloadLength / stream.PcmBytesPerSample.Value
                : payloadLength,  // fallback: treat as byte
            _ => 1
        };

    /// <summary>
    /// Returns the byte count per sample frame for a PCM stream with <c>offset_type=time</c>,
    /// or <see langword="null"/> for other types.
    /// </summary>
    private static int? ComputePcmBytesPerSample(string contentType, OffsetType offsetType)
    {
        if (offsetType != OffsetType.Time)
        {
            return null;
        }

        // Parse audio/pcm;rate=<hz>;channels=<n>;bits=<depth>
        if (!ContentTypeUtilities.TryParseContentType(contentType, out var parsed))
        {
            return null;
        }

        if (!string.Equals(parsed.MediaType, "audio/pcm", StringComparison.OrdinalIgnoreCase))
        {
            return null;
        }

        if (!parsed.Parameters.TryGetValue("channels", out var channelsStr)
            || !parsed.Parameters.TryGetValue("bits", out var bitsStr)
            || !int.TryParse(channelsStr, NumberStyles.Integer, CultureInfo.InvariantCulture, out var channels)
            || !int.TryParse(bitsStr, NumberStyles.Integer, CultureInfo.InvariantCulture, out var bits)
            || channels <= 0 || bits <= 0)
        {
            return null;
        }

        return channels * (bits / 8);
    }

    // ── Content-type matching ─────────────────────────────────────────────────

    private static bool ContentTypeMatchesStream(string frameContentType, string declaredContentType)
        => ContentTypeUtilities.ContentTypesMatch(declaredContentType, frameContentType);

    // ── String converters ─────────────────────────────────────────────────────

    private static string StreamTypeToString(CulpeoStreamType type) => type switch
    {
        CulpeoStreamType.Input => "input",
        CulpeoStreamType.Output => "output",
        CulpeoStreamType.Duplex => "duplex",
        _ => throw new ArgumentOutOfRangeException(nameof(type))
    };

    private static string OffsetTypeToString(OffsetType offsetType) => offsetType switch
    {
        OffsetType.Time => "time",
        OffsetType.Byte => "byte",
        OffsetType.Message => "message",
        _ => throw new ArgumentOutOfRangeException(nameof(offsetType))
    };

    private static bool TryParseStreamType(string? value, out CulpeoStreamType type)
    {
        switch (value)
        {
            case "input": type = CulpeoStreamType.Input; return true;
            case "output": type = CulpeoStreamType.Output; return true;
            case "duplex": type = CulpeoStreamType.Duplex; return true;
            default: type = default; return false;
        }
    }

    private static bool TryParseOffsetType(string? value, out OffsetType offsetType)
    {
        switch (value)
        {
            case "time": offsetType = OffsetType.Time; return true;
            case "byte": offsetType = OffsetType.Byte; return true;
            case "message": offsetType = OffsetType.Message; return true;
            default: offsetType = default; return false;
        }
    }
}
