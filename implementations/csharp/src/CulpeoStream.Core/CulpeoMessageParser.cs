using System.Buffers;
using System.Globalization;
using System.Text;

namespace CulpeoStream.Core;

public sealed class ParseLimits
{
    public static ParseLimits Default { get; } = new();

    public int MaxHeaderBlockSize { get; init; } = 8192;
    public int MaxHeaderCount { get; init; } = 64;
    public int MaxHeaderNameLength { get; init; } = 256;
    public int MaxHeaderValueLength { get; init; } = 4096;
}

public sealed class CulpeoMessageParser(ParseLimits limits)
{
    private static readonly SearchValues<byte> ForbiddenHeaderBytes = SearchValues.Create([(byte)'\r', (byte)'\n', (byte)0]);
    private readonly ParseLimits _limits = limits ?? throw new ArgumentNullException(nameof(limits));

    public CulpeoMessageParser() : this(ParseLimits.Default) { }

    public ValueTask<CulpeoMessage> ParseAsync(ReadOnlyMemory<byte> frameBytes, CulpeoMessageKind kind, CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();

        var searchSpan = frameBytes.Span.Length > _limits.MaxHeaderBlockSize + 4
            ? frameBytes.Span[..(_limits.MaxHeaderBlockSize + 4)]
            : frameBytes.Span;

        var terminatorIndex = searchSpan.IndexOf("\r\n\r\n"u8);
        if (terminatorIndex < 0)
        {
            throw new FormatException(
                frameBytes.Length > _limits.MaxHeaderBlockSize
                    ? "Header block exceeds maximum size."
                    : "Frame header terminator was not found.");
        }

        if (terminatorIndex > _limits.MaxHeaderBlockSize)
        {
            throw new FormatException("Header block exceeds maximum size.");
        }

        var headerBlock = frameBytes.Span[..terminatorIndex];
        var body = frameBytes[(terminatorIndex + 4)..];

        string? @event = null;
        string? contentType = null;
        string? authorization = null;
        string? sessionId = null;
        string? streamId = null;
        long? offset = null;
        long? timestamp = null;
        int? bufferWindow = null;
        string? reason = null;
        string? code = null;

        HashSet<ReservedHeader> seenHeaders = new();
        var headerCount = 0;
        var start = 0;
        while (start < headerBlock.Length)
        {
            var remaining = headerBlock[start..];
            var lineLength = remaining.IndexOf("\r\n"u8);
            if (lineLength < 0)
            {
                lineLength = remaining.Length;
            }

            var line = remaining[..lineLength];
            start += lineLength + 2;
            if (line.IsEmpty)
            {
                continue;
            }

            headerCount++;
            if (headerCount > _limits.MaxHeaderCount)
            {
                throw new FormatException($"Frame exceeds maximum header count of {_limits.MaxHeaderCount}.");
            }

            var colonIndex = line.IndexOf((byte)':');
            if (colonIndex <= 0)
            {
                throw new FormatException("Frame header is malformed.");
            }

            var name = line[..colonIndex];
            var value = TrimAsciiWhitespace(line[(colonIndex + 1)..]);

            if (name.Length > _limits.MaxHeaderNameLength)
            {
                throw new FormatException($"Header name exceeds maximum length of {_limits.MaxHeaderNameLength}.");
            }

            if (value.Length > _limits.MaxHeaderValueLength)
            {
                throw new FormatException($"Header value exceeds maximum length of {_limits.MaxHeaderValueLength}.");
            }

            ValidateNoForbiddenBytes(name, "name");
            ValidateNoForbiddenBytes(value, "value");

            if (!TryMapHeader(name, out var header))
            {
                continue;
            }

            if (!seenHeaders.Add(header))
            {
                throw new FormatException($"Duplicate reserved header: {header}.");
            }

            var decoded = Encoding.UTF8.GetString(value);
            switch (header)
            {
                case ReservedHeader.Event:
                    @event = decoded;
                    break;
                case ReservedHeader.ContentType:
                    contentType = decoded;
                    break;
                case ReservedHeader.Authorization:
                    authorization = decoded;
                    break;
                case ReservedHeader.SessionId:
                    sessionId = decoded;
                    break;
                case ReservedHeader.StreamId:
                    streamId = decoded;
                    break;
                case ReservedHeader.Offset:
                    offset = long.Parse(decoded, CultureInfo.InvariantCulture);
                    break;
                case ReservedHeader.Timestamp:
                    timestamp = long.Parse(decoded, CultureInfo.InvariantCulture);
                    break;
                case ReservedHeader.BufferWindow:
                    bufferWindow = int.Parse(decoded, CultureInfo.InvariantCulture);
                    break;
                case ReservedHeader.Reason:
                    reason = decoded;
                    break;
                case ReservedHeader.Code:
                    code = decoded;
                    break;
            }
        }

        return ValueTask.FromResult(new CulpeoMessage(
            kind,
            body,
            @event,
            contentType,
            authorization,
            sessionId,
            streamId,
            offset,
            timestamp,
            bufferWindow,
            reason,
            code));
    }

    private static void ValidateNoForbiddenBytes(ReadOnlySpan<byte> span, string fieldKind)
    {
        var forbiddenByteIndex = span.IndexOfAny(ForbiddenHeaderBytes);
        if (forbiddenByteIndex < 0)
        {
            return;
        }

        var forbiddenByte = span[forbiddenByteIndex];
        throw new FormatException($"Header {fieldKind} contains forbidden byte 0x{forbiddenByte:X2}.");
    }

    private static ReadOnlySpan<byte> TrimAsciiWhitespace(ReadOnlySpan<byte> span)
    {
        var start = 0;
        var end = span.Length - 1;

        while (start < span.Length && (span[start] == (byte)' ' || span[start] == (byte)'\t'))
        {
            start++;
        }

        while (end >= start && (span[end] == (byte)' ' || span[end] == (byte)'\t'))
        {
            end--;
        }

        return start > end ? [] : span[start..(end + 1)];
    }

    private static bool TryMapHeader(ReadOnlySpan<byte> name, out ReservedHeader header)
    {
        if (Ascii.EqualsIgnoreCase(name, "Event"u8))
        {
            header = ReservedHeader.Event;
            return true;
        }

        if (Ascii.EqualsIgnoreCase(name, "Content-Type"u8))
        {
            header = ReservedHeader.ContentType;
            return true;
        }

        if (Ascii.EqualsIgnoreCase(name, "Authorization"u8))
        {
            header = ReservedHeader.Authorization;
            return true;
        }

        if (Ascii.EqualsIgnoreCase(name, "Session-Id"u8))
        {
            header = ReservedHeader.SessionId;
            return true;
        }

        if (Ascii.EqualsIgnoreCase(name, "Stream-Id"u8))
        {
            header = ReservedHeader.StreamId;
            return true;
        }

        if (Ascii.EqualsIgnoreCase(name, "Offset"u8))
        {
            header = ReservedHeader.Offset;
            return true;
        }

        if (Ascii.EqualsIgnoreCase(name, "Timestamp"u8))
        {
            header = ReservedHeader.Timestamp;
            return true;
        }

        if (Ascii.EqualsIgnoreCase(name, "Buffer-Window"u8))
        {
            header = ReservedHeader.BufferWindow;
            return true;
        }

        if (Ascii.EqualsIgnoreCase(name, "Reason"u8))
        {
            header = ReservedHeader.Reason;
            return true;
        }

        if (Ascii.EqualsIgnoreCase(name, "Code"u8))
        {
            header = ReservedHeader.Code;
            return true;
        }

        header = default;
        return false;
    }

    private enum ReservedHeader
    {
        Event,
        ContentType,
        Authorization,
        SessionId,
        StreamId,
        Offset,
        Timestamp,
        BufferWindow,
        Reason,
        Code
    }
}
