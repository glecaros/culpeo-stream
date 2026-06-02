using System.Buffers.Binary;

namespace CulpeoStream.Http2;

/// <summary>
/// Serializes CulpeoStream frames for the HTTP/2 transport using the envelope
/// format defined in Addendum C.3–C.4 of the protocol spec:
///
/// <code>
///  +--------+--------+--------+--------+--------+------- ... ------+
///  |type (1)|      payload_length (4 bytes, big-endian)  |  payload |
///  +--------+--------+--------+--------+--------+------- ... ------+
/// </code>
///
/// The type octet precedes the 4-byte length (task-spec ordering).
/// See DECISIONS.md §"HTTP/2 frame envelope byte order" for rationale.
/// </summary>
public static class Http2FrameWriter
{
    /// <summary>
    /// Writes a single framed CulpeoStream envelope to <paramref name="stream"/>
    /// and flushes the stream.
    /// </summary>
    /// <param name="stream">The destination stream (request or response body).</param>
    /// <param name="typeOctet">
    /// Frame type: <c>0x01</c> = control/event, <c>0x02</c> = media/binary.
    /// </param>
    /// <param name="payload">
    /// Raw CulpeoStream frame bytes (header block + body, without the type or
    /// length prefix).
    /// </param>
    /// <param name="ct">Cancellation token.</param>
    public static async ValueTask WriteFrameAsync(
        Stream stream,
        byte typeOctet,
        ReadOnlyMemory<byte> payload,
        CancellationToken ct)
    {
        // 5-byte envelope header: [type:1][length:4-BE]
        // Allocate on the stack for the small fixed header.
        byte[] header = new byte[5];
        header[0] = typeOctet;
        BinaryPrimitives.WriteUInt32BigEndian(header.AsSpan(1), (uint)payload.Length);

        await stream.WriteAsync(header, ct).ConfigureAwait(false);
        await stream.WriteAsync(payload, ct).ConfigureAwait(false);
        // Flush to ensure the DATA frame is sent immediately rather than
        // sitting in a write buffer — critical for low-latency media streaming.
        await stream.FlushAsync(ct).ConfigureAwait(false);
    }
}
