using System.Buffers.Binary;
using CulpeoStream.Core;

namespace CulpeoStream.Http2;

/// <summary>
/// Deserializes CulpeoStream frames from an HTTP/2 transport stream.
/// Frame envelope format (Addendum C.3–C.4, task-spec ordering):
///
/// <code>
///  +--------+--------+--------+--------+--------+------- ... ------+
///  |type (1)|      payload_length (4 bytes, big-endian)  |  payload |
///  +--------+--------+--------+--------+--------+------- ... ------+
/// </code>
/// </summary>
public static class Http2FrameReader
{
    /// <summary>Default maximum payload size: 16 MiB.</summary>
    public const int DefaultMaxPayloadBytes = 16 * 1024 * 1024;

    /// <summary>
    /// Reads one framed CulpeoStream envelope from <paramref name="stream"/>.
    /// </summary>
    /// <param name="stream">The source stream (request or response body).</param>
    /// <param name="maxPayloadBytes">
    /// Maximum allowed payload size in bytes. Defaults to 16 MiB.
    /// </param>
    /// <param name="ct">Cancellation token.</param>
    /// <returns>The type octet and the raw payload bytes.</returns>
    /// <exception cref="EndOfStreamException">
    /// Thrown when the stream ends before a complete frame can be read.
    /// </exception>
    /// <exception cref="CulpeoProtocolException">
    /// Thrown with code <c>"frame-too-large"</c> when the declared payload
    /// length exceeds <paramref name="maxPayloadBytes"/>.
    /// </exception>
    public static async ValueTask<(byte TypeOctet, byte[] Payload)> ReadFrameAsync(
        Stream stream,
        int maxPayloadBytes = DefaultMaxPayloadBytes,
        CancellationToken ct = default)
    {
        // Read the 5-byte envelope header: [type:1][length:4-BE]
        var header = new byte[5];
        await ReadExactlyAsync(stream, header, 0, 5, ct).ConfigureAwait(false);

        var typeOctet = header[0];

        // SEC-024 fix: read as uint first, then bounds-check using uint arithmetic.
        // Casting to int before the comparison allows values >= 0x80000000 to appear
        // negative, bypassing the check and crashing or allocating 2 GiB.
        // maxPayloadBytes is a non-negative int, so (uint)maxPayloadBytes is safe.
        if (maxPayloadBytes < 0)
            throw new ArgumentOutOfRangeException(nameof(maxPayloadBytes),
                "maxPayloadBytes must be non-negative.");

        var rawLength = BinaryPrimitives.ReadUInt32BigEndian(header.AsSpan(1));

        if (rawLength > (uint)maxPayloadBytes)
        {
            throw new CulpeoProtocolException(
                "frame-too-large",
                $"Frame payload {rawLength} bytes exceeds limit {maxPayloadBytes}");
        }

        // Safe: rawLength <= maxPayloadBytes <= int.MaxValue
        var payloadLength = (int)rawLength;
        var payload = new byte[payloadLength];
        if (payloadLength > 0)
        {
            await ReadExactlyAsync(stream, payload, 0, payloadLength, ct).ConfigureAwait(false);
        }

        return (typeOctet, payload);
    }

    // ── Private helpers ────────────────────────────────────────────────────────

    /// <summary>
    /// Reads exactly <paramref name="count"/> bytes from <paramref name="stream"/>
    /// into <paramref name="buffer"/> starting at <paramref name="offset"/>.
    /// Throws <see cref="EndOfStreamException"/> if EOF is reached before
    /// all bytes are available.
    /// </summary>
    private static async ValueTask ReadExactlyAsync(
        Stream stream,
        byte[] buffer,
        int offset,
        int count,
        CancellationToken ct)
    {
        var totalRead = 0;
        while (totalRead < count)
        {
            var n = await stream
                .ReadAsync(buffer, offset + totalRead, count - totalRead, ct)
                .ConfigureAwait(false);

            if (n == 0)
            {
                throw new EndOfStreamException(
                    "Unexpected end of stream while reading a CulpeoStream HTTP/2 frame.");
            }

            totalRead += n;
        }
    }
}
