using System.Globalization;
using System.Text;

namespace CulpeoStream.Core;

public sealed class CulpeoFrameSerializer
{
    public ValueTask<byte[]> SerializeAsync(CulpeoFrame frame, CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();

        var body = frame.Kind == CulpeoFrameKind.Control && frame.Body.IsEmpty
            ? "{}"u8.ToArray()
            : frame.Body.ToArray();

        StringBuilder builder = new();
        AppendHeader(builder, "Event", frame.Event);
        AppendHeader(builder, "Content-Type", frame.ContentType);
        AppendHeader(builder, "Authorization", frame.Authorization);
        AppendHeader(builder, "Session-Id", frame.SessionId);
        AppendHeader(builder, "Stream-Id", frame.StreamId);

        if (frame.Offset.HasValue)
        {
            AppendHeader(builder, "Offset", frame.Offset.Value.ToString(CultureInfo.InvariantCulture));
        }

        if (frame.Timestamp.HasValue)
        {
            AppendHeader(builder, "Timestamp", frame.Timestamp.Value.ToString(CultureInfo.InvariantCulture));
        }

        if (frame.BufferWindow.HasValue)
        {
            AppendHeader(builder, "Buffer-Window", frame.BufferWindow.Value.ToString(CultureInfo.InvariantCulture));
        }

        AppendHeader(builder, "Reason", frame.Reason);
        AppendHeader(builder, "Code", frame.Code);
        builder.Append("\r\n");

        var headerBytes = Encoding.UTF8.GetBytes(builder.ToString());
        var output = new byte[headerBytes.Length + body.Length];
        Buffer.BlockCopy(headerBytes, 0, output, 0, headerBytes.Length);
        Buffer.BlockCopy(body, 0, output, headerBytes.Length, body.Length);
        return ValueTask.FromResult(output);
    }

    private static void AppendHeader(StringBuilder builder, string name, string? value)
    {
        if (string.IsNullOrEmpty(value))
        {
            return;
        }

        builder.Append(name);
        builder.Append(": ");
        builder.Append(value);
        builder.Append("\r\n");
    }
}
