using System.Net.WebSockets;
using System.Text;
using System.Text.Json;
using CulpeoStream.Core;

namespace CulpeoStream.AspNetCore.Tests;

/// <summary>
/// Helpers for sending and receiving CulpeoStream frames over a test WebSocket.
/// </summary>
internal static class FrameHelper
{
    private static readonly CulpeoFrameParser Parser = new();
    private static readonly CulpeoFrameSerializer Serializer = new();

    /// <summary>Sends a CulpeoStream control frame over the WebSocket.</summary>
    public static async Task SendControlFrameAsync(
        WebSocket ws,
        CulpeoFrame frame,
        CancellationToken ct = default)
    {
        var bytes = await Serializer.SerializeAsync(frame, ct);
        await ws.SendAsync(bytes.AsMemory(), WebSocketMessageType.Text, endOfMessage: true, ct);
    }

    /// <summary>Receives and parses the next frame from the WebSocket.</summary>
    public static async Task<CulpeoFrame> ReceiveFrameAsync(
        WebSocket ws,
        CancellationToken ct = default)
    {
        var buffer = new byte[8192];
        using var ms = new MemoryStream();

        WebSocketReceiveResult result;
        do
        {
            result = await ws.ReceiveAsync(new ArraySegment<byte>(buffer), ct);
            if (result.MessageType == WebSocketMessageType.Close)
            {
                throw new InvalidOperationException($"WebSocket closed: {result.CloseStatusDescription}");
            }

            ms.Write(buffer, 0, result.Count);
        } while (!result.EndOfMessage);

        var kind = result.MessageType == WebSocketMessageType.Text
            ? CulpeoFrameKind.Control
            : CulpeoFrameKind.Media;

        return await Parser.ParseAsync(ms.ToArray(), kind, ct);
    }

    /// <summary>
    /// Sends a <c>culpeo.init</c> frame and returns the server's response frame.
    /// </summary>
    public static async Task<CulpeoFrame> InitSessionAsync(
        WebSocket ws,
        string token = "Bearer test-token",
        string version = "0.3",
        string? sessionId = null,
        CancellationToken ct = default)
    {
        var streamsJson = """[{"content_type":"audio/pcm;rate=16000;channels=1;bits=16","type":"input","offset_type":"time"}]""";
        var bodyObj = sessionId is null
            ? $@"{{""version"":""{version}"",""streams"":{streamsJson}}}"
            : $@"{{""version"":""{version}"",""streams"":{streamsJson}}}";

        var initFrame = new CulpeoFrame(
            CulpeoFrameKind.Control,
            Encoding.UTF8.GetBytes(bodyObj),
            @event: "culpeo.init",
            authorization: token,
            contentType: "application/json",
            sessionId: sessionId);

        await SendControlFrameAsync(ws, initFrame, ct);
        return await ReceiveFrameAsync(ws, ct);
    }

    /// <summary>Reads the JSON body of a control frame as a <see cref="JsonDocument"/>.</summary>
    public static JsonDocument ParseBody(CulpeoFrame frame)
        => JsonDocument.Parse(frame.Body.IsEmpty ? "{}"u8.ToArray() : frame.Body.ToArray());
}
