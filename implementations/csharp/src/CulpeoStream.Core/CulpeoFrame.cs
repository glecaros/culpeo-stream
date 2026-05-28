using System.Text;

namespace CulpeoStream.Core;

public enum CulpeoFrameKind
{
    Control,
    Media
}

public enum CulpeoSessionState
{
    Uninitialized,
    Initializing,
    Established,
    Closed
}

public enum CulpeoStreamType
{
    Input,
    Output,
    Duplex
}

public sealed class CulpeoFrame
{
    public CulpeoFrame(
        CulpeoFrameKind kind,
        ReadOnlyMemory<byte> body = default,
        string? @event = null,
        string? contentType = null,
        string? authorization = null,
        string? sessionId = null,
        string? streamId = null,
        long? offset = null,
        long? timestamp = null,
        int? bufferWindow = null,
        string? reason = null,
        string? code = null)
    {
        Kind = kind;
        Body = body;
        Event = @event;
        ContentType = contentType;
        Authorization = authorization;
        SessionId = sessionId;
        StreamId = streamId;
        Offset = offset;
        Timestamp = timestamp;
        BufferWindow = bufferWindow;
        Reason = reason;
        Code = code;
    }

    public CulpeoFrameKind Kind { get; }

    public string? Event { get; }

    public string? ContentType { get; }

    public string? Authorization { get; }

    public string? SessionId { get; }

    public string? StreamId { get; }

    public long? Offset { get; }

    public long? Timestamp { get; }

    public int? BufferWindow { get; }

    public string? Reason { get; }

    public string? Code { get; }

    public ReadOnlyMemory<byte> Body { get; }

    public string GetBodyAsUtf8() => Encoding.UTF8.GetString(Body.Span);
}
