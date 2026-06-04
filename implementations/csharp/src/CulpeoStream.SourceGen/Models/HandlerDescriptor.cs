using System.Collections.Generic;
using System.Collections.Immutable;
using Microsoft.CodeAnalysis;

namespace CulpeoStream.SourceGen.Models;

/// <summary>
/// Equatable descriptor for a handler class discovered by the source generator.
/// All fields are value types / strings / ImmutableArrays to enable incremental caching.
/// </summary>
internal sealed class HandlerDescriptor : IEquatable<HandlerDescriptor>
{
    public string? Namespace { get; }
    public string ClassName { get; }
    public bool IsPartial { get; }
    public bool ImplementsHandler { get; }
    public ImmutableArray<StreamDescriptor> Streams { get; }
    public ImmutableArray<DiagnosticInfo> Diagnostics { get; }

    public HandlerDescriptor(
        string? @namespace,
        string className,
        bool isPartial,
        bool implementsHandler,
        ImmutableArray<StreamDescriptor> streams,
        ImmutableArray<DiagnosticInfo> diagnostics)
    {
        Namespace = @namespace;
        ClassName = className;
        IsPartial = isPartial;
        ImplementsHandler = implementsHandler;
        Streams = streams;
        Diagnostics = diagnostics;
    }

    public bool Equals(HandlerDescriptor? other)
    {
        if (other is null) return false;
        if (ReferenceEquals(this, other)) return true;
        return Namespace == other.Namespace
            && ClassName == other.ClassName
            && IsPartial == other.IsPartial
            && ImplementsHandler == other.ImplementsHandler
            && Streams.SequenceEqual(other.Streams)
            && Diagnostics.SequenceEqual(other.Diagnostics);
    }

    public override bool Equals(object? obj) => Equals(obj as HandlerDescriptor);

    public override int GetHashCode()
    {
        unchecked
        {
            int hash = Namespace?.GetHashCode() ?? 0;
            hash = (hash * 31) ^ ClassName.GetHashCode();
            hash = (hash * 31) ^ IsPartial.GetHashCode();
            hash = (hash * 31) ^ ImplementsHandler.GetHashCode();
            foreach (var s in Streams) hash = (hash * 31) ^ s.GetHashCode();
            foreach (var d in Diagnostics) hash = (hash * 31) ^ d.GetHashCode();
            return hash;
        }
    }
}

/// <summary>Equatable diagnostic info captured during analysis (avoids storing Roslyn <c>Location</c> in cached model).</summary>
internal sealed class DiagnosticInfo : IEquatable<DiagnosticInfo>
{
    public string Id { get; }
    public string Message { get; }
    public DiagnosticSeverity Severity { get; }
    // Location stored as primitives for equatability
    public string? FilePath { get; }
    public int Line { get; }
    public int Column { get; }
    public object[] MessageArgs { get; }

    public DiagnosticInfo(string id, string message, DiagnosticSeverity severity, string? filePath, int line, int column, object[] messageArgs)
    {
        Id = id;
        Message = message;
        Severity = severity;
        FilePath = filePath;
        Line = line;
        Column = column;
        MessageArgs = messageArgs;
    }

    public bool Equals(DiagnosticInfo? other) =>
        other is not null &&
        Id == other.Id &&
        Message == other.Message &&
        Severity == other.Severity &&
        FilePath == other.FilePath &&
        Line == other.Line &&
        Column == other.Column;

    public override bool Equals(object? obj) => Equals(obj as DiagnosticInfo);

    public override int GetHashCode()
    {
        unchecked
        {
            int hash = Id.GetHashCode();
            hash = (hash * 31) ^ Message.GetHashCode();
            hash = (hash * 31) ^ (int)Severity;
            hash = (hash * 31) ^ (FilePath?.GetHashCode() ?? 0);
            hash = (hash * 31) ^ Line;
            hash = (hash * 31) ^ Column;
            return hash;
        }
    }
}
