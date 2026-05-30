using CulpeoStream.Core;

namespace CulpeoStream.Client;

/// <summary>
/// Mutable tracking state for a single confirmed stream.
/// Not thread-safe; mutations are only performed from the receive loop.
/// </summary>
internal sealed class ClientStreamState
{
    /// <summary>Server-assigned stream identifier.</summary>
    public required string ServerId { get; set; }

    /// <summary>Declared content type (from init-ack).</summary>
    public required string ContentType { get; init; }

    /// <summary>Directionality of this stream.</summary>
    public required CulpeoStreamType Type { get; init; }

    /// <summary>Offset type for this stream.</summary>
    public required OffsetType OffsetType { get; init; }

    /// <summary>Optional semantic label.</summary>
    public string? Purpose { get; init; }

    /// <summary>
    /// For <see cref="OffsetType.Time"/> streams with <c>audio/pcm</c> content type:
    /// the number of bytes per sample frame (<c>channels × bits/8</c>).
    /// <see langword="null"/> for other offset types.
    /// </summary>
    public int? PcmBytesPerSample { get; init; }

    /// <summary>
    /// Current outgoing offset — the value to write into the next outgoing media frame's
    /// <c>Offset</c> header. Updated after each successful <see cref="CulpeoStreamClient.SendMediaAsync"/> call.
    /// </summary>
    public long SendOffset { get; set; }

    /// <summary>
    /// Highest offset advanced past for an incoming media frame on this stream.
    /// Used as the <c>resume_offset</c> hint for <c>output</c> and <c>duplex</c> streams on reconnect.
    /// </summary>
    public long ReceiveOffset { get; set; }
}
