using CulpeoStream.Core;

namespace CulpeoStream.Client;

/// <summary>
/// Declares a stream to be established in a CulpeoStream session.
/// Matches §5.2 of the CulpeoStream Protocol Specification.
/// </summary>
public sealed record StreamDeclaration
{
    /// <summary>
    /// The media type of the stream payload (e.g. <c>audio/pcm;rate=16000;channels=1;bits=16</c>).
    /// </summary>
    public required string ContentType { get; init; }

    /// <summary>
    /// Directionality of this stream: <c>input</c>, <c>output</c>, or <c>duplex</c>.
    /// </summary>
    public required CulpeoStreamType Type { get; init; }

    /// <summary>
    /// How the <c>Offset</c> value advances per frame (§5.5).
    /// </summary>
    public required OffsetType OffsetType { get; init; }

    /// <summary>
    /// Semantic label for the stream (§5.4). Required when two or more streams share the same <see cref="Type"/>.
    /// </summary>
    public string? Purpose { get; init; }
}
