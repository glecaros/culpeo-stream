using System.Net.WebSockets;
using System.Text;
using System.Text.Json;
using CulpeoStream.Core;

namespace CulpeoStream.Client.Tests;

/// <summary>
/// A pair of connected in-memory <see cref="WebSocket"/> instances.
/// The <see cref="Client"/> end is injected into <see cref="CulpeoStreamClient"/> via its test factory;
/// the <see cref="Server"/> end is driven directly from tests.
/// </summary>
internal sealed class WebSocketPair : IDisposable
{
    private readonly MemoryWebSocket _client;
    private readonly MemoryWebSocket _server;

    private static readonly CulpeoMessageParser Parser = new();
    private static readonly CulpeoMessageSerializer Serializer = new();

    private WebSocketPair(MemoryWebSocket client, MemoryWebSocket server)
    {
        _client = client;
        _server = server;
    }

    public static WebSocketPair Create()
    {
        var (client, server) = MemoryWebSocketPair.Create();
        return new WebSocketPair(client, server);
    }

    /// <summary>The WebSocket end injected into the CulpeoStreamClient under test.</summary>
    public WebSocket Client => _client;

    /// <summary>The WebSocket end driven by test server helpers.</summary>
    public WebSocket Server => _server;

    // ── Server-side helpers ───────────────────────────────────────────────────

    public async Task<CulpeoMessage> ServerReceiveAsync(CancellationToken ct = default)
    {
        var buffer = new byte[8192];
        using var ms = new MemoryStream();
        WebSocketReceiveResult result;
        do
        {
            result = await _server.ReceiveAsync(new ArraySegment<byte>(buffer), ct);
            if (result.MessageType == WebSocketMessageType.Close)
            {
                throw new InvalidOperationException("WebSocket closed unexpectedly.");
            }
            ms.Write(buffer, 0, result.Count);
        } while (!result.EndOfMessage);

        var kind = result.MessageType == WebSocketMessageType.Text
            ? CulpeoMessageKind.Control
            : CulpeoMessageKind.Media;

        return await Parser.ParseAsync(ms.ToArray(), kind, ct);
    }

    public async Task ServerSendControlAsync(CulpeoMessage frame, CancellationToken ct = default)
    {
        var bytes = await Serializer.SerializeAsync(frame, ct);
        await _server.SendAsync(bytes.AsMemory(), WebSocketMessageType.Text, endOfMessage: true, ct);
    }

    public async Task ServerSendMediaAsync(CulpeoMessage frame, CancellationToken ct = default)
    {
        var bytes = await Serializer.SerializeAsync(frame, ct);
        await _server.SendAsync(bytes.AsMemory(), WebSocketMessageType.Binary, endOfMessage: true, ct);
    }

    // ── Server-side protocol helpers ──────────────────────────────────────────

    /// <summary>
    /// Reads a <c>culpeo.init</c> from the client and responds with <c>culpeo.init-ack</c>.
    /// Returns the parsed init frame and the generated session ID.
    /// </summary>
    public async Task<(CulpeoMessage initFrame, string sessionId, List<ServerStreamInfo> streams)>
        ServerHandleInitAsync(
            string? sessionIdToReply = null,
            CancellationToken ct = default)
    {
        var initFrame = await ServerReceiveAsync(ct);

        if (!string.Equals(initFrame.Event, "culpeo.init", StringComparison.Ordinal))
        {
            throw new InvalidOperationException($"Expected culpeo.init, got '{initFrame.Event}'.");
        }

        // Parse streams from body
        using var doc = JsonDocument.Parse(initFrame.Body.ToArray());
        var root = doc.RootElement;
        var version = root.TryGetProperty("version", out var vp) ? vp.GetString() : "0.3";

        var streams = new List<ServerStreamInfo>();
        if (root.TryGetProperty("streams", out var streamsEl))
        {
            foreach (var s in streamsEl.EnumerateArray())
            {
                var id = s.TryGetProperty("id", out var idProp) ? idProp.GetString() : null;
                var ct2 = s.TryGetProperty("content_type", out var ctProp) ? ctProp.GetString() : "audio/pcm";
                var type = s.TryGetProperty("type", out var typeProp) ? typeProp.GetString() : "input";
                var ot = s.TryGetProperty("offset_type", out var otProp) ? otProp.GetString() : "message";
                var purpose = s.TryGetProperty("purpose", out var purposeProp) ? purposeProp.GetString() : null;
                long? resumeOffset = s.TryGetProperty("resume_offset", out var roProp) ? roProp.GetInt64() : null;

                streams.Add(new ServerStreamInfo(
                    ServerId: id ?? GenerateId(),
                    ContentType: ct2 ?? "audio/pcm",
                    TypeStr: type ?? "input",
                    OffsetTypeStr: ot ?? "message",
                    Purpose: purpose,
                    ResumeOffset: resumeOffset));
            }
        }

        var assignedSessionId = sessionIdToReply ?? initFrame.SessionId ?? GenerateId();

        // Build init-ack body
        var bodyBuilder = new StringBuilder();
        bodyBuilder.Append("{\"version\":\"");
        bodyBuilder.Append(version);
        bodyBuilder.Append("\",\"streams\":[");
        for (int i = 0; i < streams.Count; i++)
        {
            if (i > 0) bodyBuilder.Append(',');
            var stream = streams[i];
            bodyBuilder.Append($"{{\"id\":\"{stream.ServerId}\"");
            bodyBuilder.Append($",\"content_type\":\"{stream.ContentType}\"");
            bodyBuilder.Append($",\"type\":\"{stream.TypeStr}\"");
            bodyBuilder.Append($",\"offset_type\":\"{stream.OffsetTypeStr}\"");
            if (stream.Purpose is not null) bodyBuilder.Append($",\"purpose\":\"{stream.Purpose}\"");
            if (stream.ResumeOffset.HasValue) bodyBuilder.Append($",\"resume_offset\":{stream.ResumeOffset}");
            bodyBuilder.Append('}');
        }
        bodyBuilder.Append("]}");

        var ack = new CulpeoMessage(
            CulpeoMessageKind.Control,
            Encoding.UTF8.GetBytes(bodyBuilder.ToString()),
            @event: "culpeo.init-ack",
            contentType: "application/json",
            sessionId: assignedSessionId,
            bufferWindow: 5000);

        await ServerSendControlAsync(ack, ct);

        return (initFrame, assignedSessionId, streams);
    }

    public async Task ServerSendInitErrorAsync(string code, string reason, CancellationToken ct = default)
    {
        var frame = new CulpeoMessage(
            CulpeoMessageKind.Control,
            "{}"u8.ToArray(),
            @event: "culpeo.init-error",
            contentType: "application/json",
            code: code,
            reason: reason);

        await ServerSendControlAsync(frame, ct);
    }

    public async Task ServerSendCloseAsync(string code = "normal", string reason = "Server closing.", CancellationToken ct = default)
    {
        var frame = new CulpeoMessage(
            CulpeoMessageKind.Control,
            "{}"u8.ToArray(),
            @event: "culpeo.close",
            contentType: "application/json",
            code: code,
            reason: reason);

        await ServerSendControlAsync(frame, ct);
    }

    public async Task ServerSendAuthRefreshAsync(string nonce, CancellationToken ct = default)
    {
        var body = Encoding.UTF8.GetBytes($"{{\"nonce\":\"{nonce}\"}}");
        var frame = new CulpeoMessage(
            CulpeoMessageKind.Control,
            body,
            @event: "culpeo.auth-refresh",
            contentType: "application/json");

        await ServerSendControlAsync(frame, ct);
    }

    public async Task ServerSendMediaFrameAsync(string streamId, string contentType, long offset, byte[] payload, CancellationToken ct = default)
    {
        var frame = new CulpeoMessage(
            CulpeoMessageKind.Media,
            payload,
            contentType: contentType,
            streamId: streamId,
            offset: offset);

        await ServerSendMediaAsync(frame, ct);
    }

    private static string GenerateId()
        => Convert.ToHexString(System.Security.Cryptography.RandomNumberGenerator.GetBytes(8)).ToLowerInvariant();

    public void Dispose()
    {
        _client.Dispose();
        _server.Dispose();
    }
}

internal sealed record ServerStreamInfo(
    string ServerId,
    string ContentType,
    string TypeStr,
    string OffsetTypeStr,
    string? Purpose,
    long? ResumeOffset);
