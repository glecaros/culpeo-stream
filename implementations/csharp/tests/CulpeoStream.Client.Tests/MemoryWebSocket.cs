using System.Net.WebSockets;
using System.Threading.Channels;

namespace CulpeoStream.Client.Tests;

/// <summary>
/// In-process WebSocket pair backed by channels. Used to unit-test the client
/// without a real network connection.
///
/// Each end behaves like a real <see cref="WebSocket"/>: sends by one end are
/// received by the other.
/// </summary>
internal static class MemoryWebSocketPair
{
    /// <summary>Creates a linked pair of in-memory WebSocket endpoints.</summary>
    public static (MemoryWebSocket ClientSide, MemoryWebSocket ServerSide) Create()
    {
        // clientToServer: messages written by client, read by server
        var clientToServer = Channel.CreateUnbounded<WebSocketMessage>(
            new UnboundedChannelOptions { SingleWriter = true, SingleReader = true });

        // serverToClient: messages written by server, read by client
        var serverToClient = Channel.CreateUnbounded<WebSocketMessage>(
            new UnboundedChannelOptions { SingleWriter = true, SingleReader = true });

        var client = new MemoryWebSocket(
            outgoing: clientToServer.Writer,
            incoming: serverToClient.Reader,
            name: "client");

        var server = new MemoryWebSocket(
            outgoing: serverToClient.Writer,
            incoming: clientToServer.Reader,
            name: "server");

        return (client, server);
    }
}

internal readonly record struct WebSocketMessage(
    WebSocketMessageType Type,
    byte[] Data,
    bool EndOfMessage,
    WebSocketCloseStatus? CloseStatus = null,
    string? CloseStatusDescription = null);

/// <summary>
/// One end of an in-memory WebSocket pair. Implements the full
/// <see cref="WebSocket"/> abstract API required by <see cref="CulpeoStreamClient"/>.
/// </summary>
internal sealed class MemoryWebSocket(
    ChannelWriter<WebSocketMessage> outgoing,
    ChannelReader<WebSocketMessage> incoming,
    string name)
    : WebSocket
{
    private WebSocketState _state = WebSocketState.Open;
    private WebSocketCloseStatus? _closeStatus;
    private string? _closeStatusDescription;

    // Buffer for fragmented receive: holds the current pending message
    private byte[]? _pendingData;
    private int _pendingOffset;
    private WebSocketMessageType _pendingType;

    public override WebSocketState State => _state;
    public override WebSocketCloseStatus? CloseStatus => _closeStatus;
    public override string? CloseStatusDescription => _closeStatusDescription;
    public override string? SubProtocol => "culpeostream";

    public override void Abort()
    {
        _state = WebSocketState.Aborted;
        outgoing.TryComplete();
    }

    public override async Task CloseAsync(
        WebSocketCloseStatus closeStatus,
        string? statusDescription,
        CancellationToken cancellationToken)
    {
        if (_state == WebSocketState.Open || _state == WebSocketState.CloseReceived)
        {
            _state = WebSocketState.CloseSent;

            // Send a close message to the other end
            await outgoing.WriteAsync(new WebSocketMessage(
                WebSocketMessageType.Close,
                [],
                EndOfMessage: true,
                CloseStatus: closeStatus,
                CloseStatusDescription: statusDescription), cancellationToken)
                .ConfigureAwait(false);

            _state = WebSocketState.Closed;
            outgoing.TryComplete();
        }
    }

    public override Task CloseOutputAsync(
        WebSocketCloseStatus closeStatus,
        string? statusDescription,
        CancellationToken cancellationToken)
        => CloseAsync(closeStatus, statusDescription, cancellationToken);

    public override Task SendAsync(
        ArraySegment<byte> buffer,
        WebSocketMessageType messageType,
        bool endOfMessage,
        CancellationToken cancellationToken)
    {
        ThrowIfNotOpen();
        var data = buffer.Array is null ? [] : buffer.ToArray();
        var msg = new WebSocketMessage(messageType, data, endOfMessage);
        return outgoing.WriteAsync(msg, cancellationToken).AsTask();
    }

    // Memory<byte> overload (used by newer BCL code)
    public override ValueTask SendAsync(
        ReadOnlyMemory<byte> buffer,
        WebSocketMessageType messageType,
        bool endOfMessage,
        CancellationToken cancellationToken)
    {
        ThrowIfNotOpen();
        var data = buffer.ToArray();
        var msg = new WebSocketMessage(messageType, data, endOfMessage);
        return outgoing.WriteAsync(msg, cancellationToken);
    }

    public override async Task<WebSocketReceiveResult> ReceiveAsync(
        ArraySegment<byte> buffer,
        CancellationToken cancellationToken)
    {
        // If we have buffered data from a previous partial read, serve from it
        if (_pendingData is not null)
        {
            return CopyFromPending(buffer);
        }

        // Wait for next message
        WebSocketMessage msg;
        try
        {
            msg = await incoming.ReadAsync(cancellationToken).ConfigureAwait(false);
        }
        catch (ChannelClosedException)
        {
            _state = WebSocketState.Closed;
            return new WebSocketReceiveResult(0, WebSocketMessageType.Close, true,
                WebSocketCloseStatus.NormalClosure, "Channel closed.");
        }

        if (msg.Type == WebSocketMessageType.Close)
        {
            _closeStatus = msg.CloseStatus ?? WebSocketCloseStatus.NormalClosure;
            _closeStatusDescription = msg.CloseStatusDescription ?? "Connection closed.";
            _state = WebSocketState.CloseReceived;
            return new WebSocketReceiveResult(0, WebSocketMessageType.Close, true,
                _closeStatus, _closeStatusDescription);
        }

        // Store message and serve it
        _pendingData = msg.Data;
        _pendingOffset = 0;
        _pendingType = msg.Type;
        return CopyFromPending(buffer);
    }

    private WebSocketReceiveResult CopyFromPending(ArraySegment<byte> buffer)
    {
        var data = _pendingData!;
        var remaining = data.Length - _pendingOffset;
        var toCopy = Math.Min(remaining, buffer.Count);

        Buffer.BlockCopy(data, _pendingOffset, buffer.Array!, buffer.Offset, toCopy);
        _pendingOffset += toCopy;

        var endOfMessage = _pendingOffset >= data.Length;
        if (endOfMessage)
        {
            _pendingData = null;
            _pendingOffset = 0;
        }

        return new WebSocketReceiveResult(toCopy, _pendingType, endOfMessage);
    }

    public override void Dispose()
    {
        _state = WebSocketState.Closed;
        outgoing.TryComplete();
    }

    private void ThrowIfNotOpen()
    {
        if (_state != WebSocketState.Open)
        {
            throw new WebSocketException(WebSocketError.InvalidState,
                $"WebSocket '{name}' is not open (state={_state}).");
        }
    }
}
