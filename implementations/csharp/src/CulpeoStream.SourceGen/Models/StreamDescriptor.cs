using System.Collections.Generic;
using System.Collections.Immutable;

namespace CulpeoStream.SourceGen.Models;

/// <summary>
/// Equatable descriptor for a single stream declared via <c>[DeclareStream]</c>.
/// </summary>
internal sealed class StreamDescriptor : IEquatable<StreamDescriptor>
{
    /// <summary>Stream ID as supplied in the attribute, e.g. <c>"audio-in"</c>.</summary>
    public string Id { get; }

    /// <summary>MIME content type, e.g. <c>"audio/pcm;rate=16000;channels=1;bits=16"</c>.</summary>
    public string ContentType { get; }

    /// <summary>Integer value of <c>CulpeoStreamType</c> enum: 0=Input, 1=Output, 2=Duplex.</summary>
    public int StreamType { get; }

    /// <summary>Integer value of <c>OffsetType</c> enum: 0=Time, 1=Byte, 2=Message.</summary>
    public int OffsetType { get; }

    /// <summary>Optional semantic label.</summary>
    public string? Purpose { get; }

    /// <summary>The C# field name the attribute was applied to (used for diagnostics).</summary>
    public string FieldName { get; }

    // Location stored as primitives for equatability
    public string? FilePath { get; }
    public int Line { get; }
    public int Column { get; }

    public StreamDescriptor(
        string id,
        string contentType,
        int streamType,
        int offsetType,
        string? purpose,
        string fieldName,
        string? filePath,
        int line,
        int column)
    {
        Id = id;
        ContentType = contentType;
        StreamType = streamType;
        OffsetType = offsetType;
        Purpose = purpose;
        FieldName = fieldName;
        FilePath = filePath;
        Line = line;
        Column = column;
    }

    /// <summary>
    /// Returns a C#-safe identifier segment derived from the stream ID
    /// (e.g. <c>"audio-in"</c> → <c>"AudioIn"</c>).
    /// </summary>
    public string SafeMethodSuffix
    {
        get
        {
            var sb = new System.Text.StringBuilder();
            bool capitalizeNext = true;
            foreach (char c in Id)
            {
                if (c == '-' || c == '_' || c == '.')
                {
                    capitalizeNext = true;
                }
                else if (char.IsLetterOrDigit(c))
                {
                    sb.Append(capitalizeNext ? char.ToUpperInvariant(c) : c);
                    capitalizeNext = false;
                }
            }
            return sb.Length == 0 ? "Stream" : sb.ToString();
        }
    }

    public bool Equals(StreamDescriptor? other) =>
        other is not null &&
        Id == other.Id &&
        ContentType == other.ContentType &&
        StreamType == other.StreamType &&
        OffsetType == other.OffsetType &&
        Purpose == other.Purpose &&
        FieldName == other.FieldName;

    public override bool Equals(object? obj) => Equals(obj as StreamDescriptor);

    public override int GetHashCode()
    {
        unchecked
        {
            int hash = Id.GetHashCode();
            hash = (hash * 31) ^ ContentType.GetHashCode();
            hash = (hash * 31) ^ StreamType;
            hash = (hash * 31) ^ OffsetType;
            hash = (hash * 31) ^ (Purpose?.GetHashCode() ?? 0);
            hash = (hash * 31) ^ FieldName.GetHashCode();
            return hash;
        }
    }
}
