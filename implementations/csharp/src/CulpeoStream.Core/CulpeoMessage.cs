using System.Text;

namespace CulpeoStream.Core;

/// <summary>
/// Declares a stream to be established in a CulpeoStream session (§5.2).
/// Used in <c>culpeo.init</c> and returned by source-generated <c>RegisteredStreams</c>.
/// </summary>
public sealed record StreamDeclaration
{
    /// <summary>The media type of the stream payload (e.g. <c>audio/pcm;rate=16000;channels=1;bits=16</c>).</summary>
    public required string ContentType { get; init; }

    /// <summary>Directionality of this stream: <c>input</c>, <c>output</c>, or <c>duplex</c>.</summary>
    public required CulpeoStreamType Type { get; init; }

    /// <summary>How the <c>Offset</c> value advances per frame (§5.5).</summary>
    public required OffsetType OffsetType { get; init; }

    /// <summary>Semantic label for the stream (§5.4). Required when two or more streams share the same <see cref="Type"/>.</summary>
    public string? Purpose { get; init; }
}

public enum CulpeoMessageKind
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

public sealed class CulpeoMessage
{
    public CulpeoMessage(
        CulpeoMessageKind kind,
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

    public CulpeoMessageKind Kind { get; }

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
