using System.Buffers;
using System.Collections.ObjectModel;
using System.Globalization;
using System.Security.Cryptography;
using System.Text.Json;

namespace CulpeoStream.Core;

public sealed class CulpeoSessionOptions
{
    public IReadOnlyList<string> SupportedVersions { get; init; } = ["0.3"];

    public int MaxBufferWindowMs { get; init; } = 30_000;

    public int MaxStreamCount { get; init; } = 16;

    public TimeSpan AuthChallengeTimeout { get; init; } = TimeSpan.FromSeconds(30);

    public TimeProvider TimeProvider { get; init; } = TimeProvider.System;

    public int SessionIdByteLength { get; init; } = 16;

    public int NonceByteLength { get; init; } = 16;
}

public sealed class CulpeoProcessResult
{
    public static CulpeoProcessResult Empty(CulpeoSessionState state) => new([], false, null, state);

    public CulpeoProcessResult(IReadOnlyList<CulpeoMessage> outboundFrames, bool shouldClose, string? closeCode, CulpeoSessionState state)
    {
        OutboundFrames = outboundFrames;
        ShouldClose = shouldClose;
        CloseCode = closeCode;
        State = state;
    }

    public IReadOnlyList<CulpeoMessage> OutboundFrames { get; }

    public bool ShouldClose { get; }

    public string? CloseCode { get; }

    public CulpeoSessionState State { get; }
}

/// <summary>Declares how the <c>Offset</c> value on media frames advances for a stream.</summary>
public enum OffsetType
{
    /// <summary>Offset increments by sample count per channel (PCM formula). Requires <c>audio/pcm</c> content type.</summary>
    Time,
    /// <summary>Offset increments by the raw byte length of the media payload.</summary>
    Byte,
    /// <summary>Offset increments by 1 per delivered media frame.</summary>
    Message,
}

public sealed class CulpeoStreamInfo(string id, string contentType, CulpeoStreamType type, string? purpose, long currentOffset, OffsetType offsetType)
{
    public string Id { get; } = id;

    public string ContentType { get; } = contentType;

    public CulpeoStreamType Type { get; } = type;

    public string? Purpose { get; } = purpose;

    public long CurrentOffset { get; } = currentOffset;

    public OffsetType OffsetType { get; } = offsetType;
}

public sealed class CulpeoSessionServer(CulpeoSessionOptions? options = null)
{
    private readonly object sync = new();
    private readonly Dictionary<string, SessionSnapshot> sessions = new(StringComparer.Ordinal);

    public CulpeoSessionOptions Options { get; } = CreateOptions(options);

    public ValueTask<CulpeoConnection> CreateConnectionAsync(CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(new CulpeoConnection(this));
    }

    internal SessionSnapshot? GetSession(string sessionId)
    {
        lock (sync)
        {
            sessions.TryGetValue(sessionId, out var snapshot);
            return snapshot;
        }
    }

    internal void SaveSession(SessionSnapshot snapshot)
    {
        lock (sync)
        {
            sessions[snapshot.SessionId] = snapshot;
        }
    }

    internal void RemoveSession(string sessionId)
    {
        lock (sync)
        {
            sessions.Remove(sessionId);
        }
    }

    private static CulpeoSessionOptions CreateOptions(CulpeoSessionOptions? options)
    {
        var resolvedOptions = options ?? new CulpeoSessionOptions();
        if (resolvedOptions.SupportedVersions.Count == 0)
        {
            throw new ArgumentException("At least one supported version is required.", nameof(options));
        }

        return resolvedOptions;
    }
}

public sealed class CulpeoConnection(CulpeoSessionServer server)
{
    private readonly HashSet<string> issuedNonces = new(StringComparer.Ordinal);
    private readonly Queue<DateTimeOffset> pingTimestamps = new();
    private SessionSnapshot? snapshot;
    private string? pendingNonce;
    private DateTimeOffset? authChallengeIssuedAt;

    public CulpeoSessionState State { get; private set; } = CulpeoSessionState.Uninitialized;

    public string? SessionId => snapshot?.SessionId;

    public IReadOnlyList<CulpeoStreamInfo> Streams => snapshot is null
        ? []
        : new ReadOnlyCollection<CulpeoStreamInfo>(snapshot.Streams.Values
            .Select(stream => new CulpeoStreamInfo(stream.Id, stream.ContentType, stream.Type, stream.Purpose, stream.CurrentOffset, stream.OffsetType))
            .ToList());

    public ValueTask<CulpeoProcessResult> ReceiveAsync(CulpeoMessage frame, CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();

        var timeoutResult = CheckPendingAuthTimeout();
        if (timeoutResult is not null)
        {
            return ValueTask.FromResult(timeoutResult);
        }

        return State switch
        {
            CulpeoSessionState.Uninitialized => ValueTask.FromResult(HandleInitialFrame(frame)),
            CulpeoSessionState.Initializing => ValueTask.FromResult(CloseWithCloseFrame("protocol-error", "Session initialization is already in progress.")),
            CulpeoSessionState.Established => ValueTask.FromResult(HandleEstablishedFrame(frame)),
            _ => ValueTask.FromResult(new CulpeoProcessResult([], true, "closed", CulpeoSessionState.Closed))
        };
    }

    public ValueTask<CulpeoMessage> SendMediaAsync(string streamId, ReadOnlyMemory<byte> payload, long timestamp, CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        EnsureEstablished();

        var stream = GetStreamOrThrow(streamId);
        if (stream.Type == CulpeoStreamType.Input)
        {
            throw new InvalidOperationException("Server cannot send media on an input stream.");
        }

        var offset = stream.CurrentOffset;
        stream.RecordFrame(offset, server.Options.TimeProvider.GetUtcNow());
        stream.AdvanceOffset(payload.Length);

        return ValueTask.FromResult(new CulpeoMessage(
            CulpeoMessageKind.Media,
            payload,
            contentType: stream.ContentType,
            streamId: stream.Id,
            offset: offset,
            timestamp: timestamp));
    }

    public ValueTask<CulpeoMessage> IssueAuthRefreshAsync(CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        EnsureEstablished();

        var nonce = GenerateOpaqueId(server.Options.NonceByteLength);
        issuedNonces.Add(nonce);
        pendingNonce = nonce;
        authChallengeIssuedAt = server.Options.TimeProvider.GetUtcNow();

        return ValueTask.FromResult(new CulpeoMessage(
            CulpeoMessageKind.Control,
            SerializeJsonBody(writer => writer.WriteString("nonce", nonce)),
            @event: "culpeo.auth-refresh",
            contentType: "application/json"));
    }

    public ValueTask<CulpeoProcessResult> CheckTimeoutsAsync(CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(CheckPendingAuthTimeout() ?? CulpeoProcessResult.Empty(State));
    }

    public ValueTask DisconnectAsync(CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (snapshot is not null)
        {
            snapshot.DisconnectedAt = server.Options.TimeProvider.GetUtcNow();
            server.SaveSession(snapshot);
        }

        State = CulpeoSessionState.Closed;
        return ValueTask.CompletedTask;
    }

    private CulpeoProcessResult HandleInitialFrame(CulpeoMessage frame)
    {
        if (frame.Kind != CulpeoMessageKind.Control || !string.Equals(frame.Event, "culpeo.init", StringComparison.Ordinal))
        {
            return CloseWithCloseFrame("protocol-error", "The first frame must be culpeo.init.");
        }

        State = CulpeoSessionState.Initializing;

        if (string.IsNullOrWhiteSpace(frame.Authorization))
        {
            return CloseWithInitError("unauthorized", "Authorization is required.", null);
        }

        try
        {
            var init = ParseInitBody(frame.Body.Span);
            if (!server.Options.SupportedVersions.Contains(init.Version, StringComparer.Ordinal))
            {
                return CloseWithInitError(
                    "unsupported-version",
                    "Protocol version not supported",
                    SerializeJsonBody(writer =>
                    {
                        writer.WriteStartArray("supported_versions");
                        foreach (var version in server.Options.SupportedVersions)
                        {
                            writer.WriteStringValue(version);
                        }
                        writer.WriteEndArray();
                    }));
            }

            var requestedBufferWindow = frame.BufferWindow ?? server.Options.MaxBufferWindowMs;
            var negotiatedBufferWindow = Math.Min(Math.Max(0, requestedBufferWindow), server.Options.MaxBufferWindowMs);

            snapshot = string.IsNullOrWhiteSpace(frame.SessionId)
                ? CreateNewSession(init, negotiatedBufferWindow)
                : ResumeSession(frame.SessionId!, init, negotiatedBufferWindow);

            server.SaveSession(snapshot);
            State = CulpeoSessionState.Established;
            return new CulpeoProcessResult([CreateInitAckFrame(snapshot, init.Version, negotiatedBufferWindow, init.IsResumption)], false, null, State);
        }
        catch (ProtocolValidationException ex)
        {
            return CloseWithInitError(ex.Code, ex.Reason, ex.Body);
        }
    }

    private CulpeoProcessResult HandleEstablishedFrame(CulpeoMessage frame)
    {
        if (frame.Kind == CulpeoMessageKind.Media)
        {
            return HandleIncomingMedia(frame);
        }

        try
        {
            return frame.Event switch
            {
                "culpeo.ping" => HandlePing(frame),
                "culpeo.auth-response" => HandleAuthResponse(frame),
                "culpeo.init" => CloseWithCloseFrame("protocol-error", "culpeo.init is not valid after session establishment."),
                "culpeo.close" => HandleClose(frame),
                _ => CulpeoProcessResult.Empty(State)
            };
        }
        catch (JsonException)
        {
            return CloseWithCloseFrame("protocol-error", "Control frame body must be valid JSON.");
        }
        catch (ProtocolValidationException ex)
        {
            return CloseWithCloseFrame(ex.Code, ex.Reason);
        }
    }

    private CulpeoProcessResult HandlePing(CulpeoMessage frame)
    {
        using var document = JsonDocument.Parse(frame.Body.IsEmpty ? "{}"u8.ToArray() : frame.Body.ToArray());
        if (!document.RootElement.TryGetProperty("ts", out var tsProperty) || !tsProperty.TryGetInt64(out var ts))
        {
            throw new ProtocolValidationException("protocol-error", "culpeo.ping must include ts.");
        }

        var now = server.Options.TimeProvider.GetUtcNow();
        if (!TryTrackPing(now))
        {
            return CulpeoProcessResult.Empty(State);
        }

        var serverTs = now.ToUnixTimeMilliseconds() * 1000;
        var pong = new CulpeoMessage(
            CulpeoMessageKind.Control,
            SerializeJsonBody(writer =>
            {
                writer.WriteNumber("ts", ts);
                writer.WriteNumber("server_ts", serverTs);
            }),
            @event: "culpeo.pong",
            contentType: "application/json");

        return new CulpeoProcessResult([pong], false, null, State);
    }

    private CulpeoProcessResult HandleAuthResponse(CulpeoMessage frame)
    {
        if (string.IsNullOrWhiteSpace(frame.Authorization))
        {
            throw new ProtocolValidationException("unauthorized", "Authorization is required for auth-response.");
        }

        using var document = JsonDocument.Parse(frame.Body.IsEmpty ? "{}"u8.ToArray() : frame.Body.ToArray());
        if (!document.RootElement.TryGetProperty("nonce", out var nonceProperty))
        {
            throw new ProtocolValidationException("unauthorized", "Nonce is required for auth-response.");
        }

        var nonce = nonceProperty.GetString();
        if (string.IsNullOrWhiteSpace(nonce) || pendingNonce is null || !issuedNonces.Remove(nonce) || !string.Equals(nonce, pendingNonce, StringComparison.Ordinal))
        {
            throw new ProtocolValidationException("unauthorized", "Invalid auth refresh nonce.");
        }

        pendingNonce = null;
        authChallengeIssuedAt = null;
        return CulpeoProcessResult.Empty(State);
    }

    private CulpeoProcessResult HandleClose(CulpeoMessage frame)
    {
        State = CulpeoSessionState.Closed;
        if (snapshot is not null)
        {
            snapshot.DisconnectedAt = server.Options.TimeProvider.GetUtcNow();
            server.SaveSession(snapshot);
        }

        var close = new CulpeoMessage(
            CulpeoMessageKind.Control,
            SerializeJsonBody(_ => { }),
            @event: "culpeo.close",
            contentType: "application/json",
            code: frame.Code ?? "normal-closure",
            reason: frame.Reason ?? "Session closed");

        return new CulpeoProcessResult([close], true, frame.Code ?? "normal-closure", State);
    }

    private CulpeoProcessResult HandleIncomingMedia(CulpeoMessage frame)
    {
        try
        {
            if (string.IsNullOrWhiteSpace(frame.StreamId))
            {
                throw new ProtocolValidationException("protocol-error", "Media frames must include Stream-Id.");
            }

            var stream = GetStreamOrThrow(frame.StreamId);
            if (stream.Type == CulpeoStreamType.Output)
            {
                throw new ProtocolValidationException("protocol-error", "Client cannot send media on an output stream.");
            }

            if (string.IsNullOrWhiteSpace(frame.ContentType) || !ContentTypeUtilities.ContentTypesMatch(stream.ContentType, frame.ContentType))
            {
                throw new ProtocolValidationException("protocol-error", "Media frame content type does not match the declared stream.");
            }

            if (!frame.Offset.HasValue)
            {
                throw new ProtocolValidationException("protocol-error", "Media frames must include Offset.");
            }

            if (frame.Offset.Value != stream.CurrentOffset)
            {
                throw new ProtocolValidationException("protocol-error", "Media frame offset is out of sequence.");
            }

            stream.RecordFrame(frame.Offset.Value, server.Options.TimeProvider.GetUtcNow());
            stream.AdvanceOffset(frame.Body.Length);
            return CulpeoProcessResult.Empty(State);
        }
        catch (ProtocolValidationException ex)
        {
            return CloseWithCloseFrame(ex.Code, ex.Reason);
        }
    }

    private SessionSnapshot CreateNewSession(InitRequest init, int bufferWindow)
    {
        ValidateStreamDeclarations(init.Streams, server.Options.MaxStreamCount);
        var sessionId = GenerateOpaqueId(server.Options.SessionIdByteLength);
        var streams = init.Streams
            .Select(stream => new StreamState(GenerateOpaqueId(8), stream.ContentType, stream.Type, stream.Purpose, 0, stream.OffsetType))
            .ToDictionary(stream => stream.Id, StringComparer.Ordinal);

        return new SessionSnapshot(sessionId, bufferWindow, streams);
    }

    private SessionSnapshot ResumeSession(string sessionId, InitRequest init, int bufferWindow)
    {
        ValidateStreamDeclarations(init.Streams, server.Options.MaxStreamCount);

        var existing = server.GetSession(sessionId);
        if (existing is null)
        {
            throw new ProtocolValidationException("invalid-session", "Session does not exist.");
        }

        if (existing.DisconnectedAt.HasValue && server.Options.TimeProvider.GetUtcNow() - existing.DisconnectedAt.Value > TimeSpan.FromMilliseconds(existing.BufferWindowMs))
        {
            server.RemoveSession(sessionId);
            throw new ProtocolValidationException("invalid-session", "Session has expired.");
        }

        HashSet<string> matched = new(StringComparer.Ordinal);
        foreach (var declaration in init.Streams)
        {
            var existingStream = FindExistingStream(existing, declaration, matched);
            if (existingStream is null)
            {
                throw new ProtocolValidationException("invalid-streams", "Resumption stream declarations do not match the existing session.");
            }

            var requestedOffset = declaration.ResumeOffset ?? existingStream.CurrentOffset;
            if (requestedOffset > existingStream.CurrentOffset)
            {
                throw new ProtocolValidationException("invalid-streams", "resume_offset cannot exceed the current stream offset.");
            }

            var earliest = existingStream.GetEarliestAvailableOffset(TimeSpan.FromMilliseconds(bufferWindow), server.Options.TimeProvider.GetUtcNow());
            existingStream.ConfirmedResumeOffset = Math.Max(requestedOffset, earliest);
            matched.Add(existingStream.Id);
        }

        existing.BufferWindowMs = bufferWindow;
        existing.DisconnectedAt = null;
        return existing;
    }

    private bool TryTrackPing(DateTimeOffset now)
    {
        var cutoff = now - TimeSpan.FromSeconds(1);
        while (pingTimestamps.Count > 0 && pingTimestamps.Peek() <= cutoff)
        {
            pingTimestamps.Dequeue();
        }

        if (pingTimestamps.Count >= 5)
        {
            return false;
        }

        pingTimestamps.Enqueue(now);
        return true;
    }

    private static void ValidateStreamDeclarations(IReadOnlyList<StreamDeclaration> streams, int maxStreamCount)
    {
        if (streams.Count == 0)
        {
            throw new ProtocolValidationException("invalid-streams", "At least one stream must be declared.");
        }

        if (streams.Count > maxStreamCount)
        {
            throw new ProtocolValidationException("invalid-streams", $"A maximum of {maxStreamCount} streams may be declared.");
        }

        foreach (var group in streams.GroupBy(stream => stream.Type))
        {
            if (group.Count() < 2)
            {
                continue;
            }

            HashSet<string> purposes = new(StringComparer.Ordinal);
            foreach (var stream in group)
            {
                if (string.IsNullOrWhiteSpace(stream.Purpose) || !purposes.Add(stream.Purpose))
                {
                    throw new ProtocolValidationException("invalid-streams", "Stream purposes must be present and unique within a type.");
                }
            }
        }
    }

    private static StreamState? FindExistingStream(SessionSnapshot snapshot, StreamDeclaration declaration, HashSet<string> matched)
    {
        if (!string.IsNullOrWhiteSpace(declaration.IdHint) && snapshot.Streams.TryGetValue(declaration.IdHint, out var byId) && !matched.Contains(byId.Id) && MatchesStream(byId, declaration))
        {
            return byId;
        }

        return snapshot.Streams.Values.FirstOrDefault(stream => !matched.Contains(stream.Id) && MatchesStream(stream, declaration));
    }

    private static bool MatchesStream(StreamState stream, StreamDeclaration declaration)
        => stream.Type == declaration.Type
           && stream.OffsetType == declaration.OffsetType
           && ContentTypeUtilities.ContentTypesMatch(stream.ContentType, declaration.ContentType)
           && string.Equals(stream.Purpose, declaration.Purpose, StringComparison.Ordinal);

    private CulpeoMessage CreateInitAckFrame(SessionSnapshot currentSnapshot, string version, int bufferWindow, bool resumption)
    {
        return new CulpeoMessage(
            CulpeoMessageKind.Control,
            SerializeJsonBody(writer =>
            {
                writer.WriteString("version", version);
                writer.WriteStartArray("streams");
                foreach (var stream in currentSnapshot.Streams.Values)
                {
                    writer.WriteStartObject();
                    writer.WriteString("id", stream.Id);
                    writer.WriteString("content_type", stream.ContentType);
                    writer.WriteString("type", StreamTypeToString(stream.Type));
                    writer.WriteString("offset_type", OffsetTypeToString(stream.OffsetType));
                    if (!string.IsNullOrWhiteSpace(stream.Purpose))
                    {
                        writer.WriteString("purpose", stream.Purpose);
                    }

                    if (resumption)
                    {
                        writer.WriteNumber("resume_offset", stream.ConfirmedResumeOffset);
                    }

                    writer.WriteEndObject();
                }
                writer.WriteEndArray();
            }),
            @event: "culpeo.init-ack",
            contentType: "application/json",
            sessionId: currentSnapshot.SessionId,
            bufferWindow: bufferWindow);
    }

    private CulpeoProcessResult? CheckPendingAuthTimeout()
    {
        if (pendingNonce is null || authChallengeIssuedAt is null)
        {
            return null;
        }

        if (server.Options.TimeProvider.GetUtcNow() - authChallengeIssuedAt.Value <= server.Options.AuthChallengeTimeout)
        {
            return null;
        }

        pendingNonce = null;
        authChallengeIssuedAt = null;
        State = CulpeoSessionState.Closed;
        return new CulpeoProcessResult(
            [
                new CulpeoMessage(
                    CulpeoMessageKind.Control,
                    SerializeJsonBody(_ => { }),
                    @event: "culpeo.close",
                    contentType: "application/json",
                    code: "auth-expired",
                    reason: "Authentication refresh timed out.")
            ],
            true,
            "auth-expired",
            State);
    }

    private CulpeoProcessResult CloseWithInitError(string code, string reason, ReadOnlyMemory<byte>? body)
    {
        State = CulpeoSessionState.Closed;
        return new CulpeoProcessResult(
            [
                new CulpeoMessage(
                    CulpeoMessageKind.Control,
                    body ?? SerializeJsonBody(_ => { }),
                    @event: "culpeo.init-error",
                    contentType: "application/json",
                    code: code,
                    reason: reason)
            ],
            true,
            code,
            State);
    }

    private CulpeoProcessResult CloseWithCloseFrame(string code, string reason)
    {
        State = CulpeoSessionState.Closed;
        if (snapshot is not null)
        {
            snapshot.DisconnectedAt = server.Options.TimeProvider.GetUtcNow();
            server.SaveSession(snapshot);
        }

        return new CulpeoProcessResult(
            [
                new CulpeoMessage(
                    CulpeoMessageKind.Control,
                    SerializeJsonBody(_ => { }),
                    @event: "culpeo.close",
                    contentType: "application/json",
                    code: code,
                    reason: reason)
            ],
            true,
            code,
            State);
    }

    private static InitRequest ParseInitBody(ReadOnlySpan<byte> body)
    {
        using var document = JsonDocument.Parse(body.IsEmpty ? "{}"u8.ToArray() : body.ToArray());
        var root = document.RootElement;

        if (!root.TryGetProperty("version", out var versionProperty) || string.IsNullOrWhiteSpace(versionProperty.GetString()))
        {
            throw new ProtocolValidationException("protocol-error", "culpeo.init must include version.");
        }

        if (!root.TryGetProperty("streams", out var streamsProperty) || streamsProperty.ValueKind != JsonValueKind.Array)
        {
            throw new ProtocolValidationException("invalid-streams", "culpeo.init must include streams.");
        }

        List<StreamDeclaration> streams = [];
        foreach (var item in streamsProperty.EnumerateArray())
        {
            var contentType = item.TryGetProperty("content_type", out var contentTypeProperty) ? contentTypeProperty.GetString() : null;
            var typeValue = item.TryGetProperty("type", out var typeProperty) ? typeProperty.GetString() : null;
            if (string.IsNullOrWhiteSpace(contentType) || string.IsNullOrWhiteSpace(typeValue) || !TryParseStreamType(typeValue, out var streamType))
            {
                throw new ProtocolValidationException("invalid-streams", "Each stream must declare content_type and a valid type.");
            }

            var offsetTypeValue = item.TryGetProperty("offset_type", out var offsetTypeProperty) ? offsetTypeProperty.GetString() : null;
            if (string.IsNullOrWhiteSpace(offsetTypeValue) || !TryParseOffsetType(offsetTypeValue, out var offsetType))
            {
                throw new ProtocolValidationException("invalid-streams", "Each stream must declare a valid offset_type (one of: time, byte, message).");
            }

            long? resumeOffset = null;
            if (item.TryGetProperty("resume_offset", out var resumeOffsetProperty))
            {
                resumeOffset = resumeOffsetProperty.GetInt64();
            }

            streams.Add(new StreamDeclaration(
                contentType,
                streamType,
                item.TryGetProperty("purpose", out var purposeProperty) ? purposeProperty.GetString() : null,
                item.TryGetProperty("id", out var idProperty) ? idProperty.GetString() : null,
                resumeOffset,
                offsetType));
        }

        return new InitRequest(versionProperty.GetString()!, streams);
    }

    private StreamState GetStreamOrThrow(string streamId)
    {
        EnsureEstablished();
        if (!snapshot!.Streams.TryGetValue(streamId, out var stream))
        {
            throw new ProtocolValidationException("protocol-error", "The referenced stream does not exist.");
        }

        return stream;
    }

    private void EnsureEstablished()
    {
        if (State != CulpeoSessionState.Established || snapshot is null)
        {
            throw new InvalidOperationException("The session is not established.");
        }
    }

    private static string GenerateOpaqueId(int byteLength)
        => Convert.ToHexString(RandomNumberGenerator.GetBytes(byteLength)).ToLowerInvariant();

    private static ReadOnlyMemory<byte> SerializeJsonBody(Action<Utf8JsonWriter> writeBody)
    {
        var buffer = new ArrayBufferWriter<byte>();
        using var writer = new Utf8JsonWriter(buffer);
        writer.WriteStartObject();
        writeBody(writer);
        writer.WriteEndObject();
        writer.Flush();
        return buffer.WrittenMemory.ToArray();
    }

    private static bool TryParseStreamType(string value, out CulpeoStreamType type)
    {
        switch (value)
        {
            case "input":
                type = CulpeoStreamType.Input;
                return true;
            case "output":
                type = CulpeoStreamType.Output;
                return true;
            case "duplex":
                type = CulpeoStreamType.Duplex;
                return true;
            default:
                type = default;
                return false;
        }
    }

    private static string StreamTypeToString(CulpeoStreamType type) => type switch
    {
        CulpeoStreamType.Input => "input",
        CulpeoStreamType.Output => "output",
        CulpeoStreamType.Duplex => "duplex",
        _ => throw new ArgumentOutOfRangeException(nameof(type))
    };

    private static bool TryParseOffsetType(string value, out OffsetType offsetType)
    {
        switch (value)
        {
            case "time":
                offsetType = OffsetType.Time;
                return true;
            case "byte":
                offsetType = OffsetType.Byte;
                return true;
            case "message":
                offsetType = OffsetType.Message;
                return true;
            default:
                offsetType = default;
                return false;
        }
    }

    private static string OffsetTypeToString(OffsetType offsetType) => offsetType switch
    {
        OffsetType.Time => "time",
        OffsetType.Byte => "byte",
        OffsetType.Message => "message",
        _ => throw new ArgumentOutOfRangeException(nameof(offsetType))
    };
}

internal readonly record struct ParsedContentType(string MediaType, IReadOnlyDictionary<string, string> Parameters);

internal static class ContentTypeUtilities
{
    public static bool ContentTypesMatch(string declaredContentType, string actualContentType)
    {
        if (!TryParseContentType(declaredContentType, out var declared) || !TryParseContentType(actualContentType, out var actual))
        {
            return string.Equals(declaredContentType, actualContentType, StringComparison.Ordinal);
        }

        if (!string.Equals(declared.MediaType, actual.MediaType, StringComparison.OrdinalIgnoreCase)
            || declared.Parameters.Count != actual.Parameters.Count)
        {
            return false;
        }

        foreach (var parameter in declared.Parameters)
        {
            if (!actual.Parameters.TryGetValue(parameter.Key, out var actualValue)
                || !string.Equals(parameter.Value, actualValue, StringComparison.Ordinal))
            {
                return false;
            }
        }

        return true;
    }

    public static bool TryParseContentType(string contentType, out ParsedContentType parsed)
    {
        var parts = contentType.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        if (parts.Length == 0 || string.IsNullOrWhiteSpace(parts[0]))
        {
            parsed = default;
            return false;
        }

        Dictionary<string, string> parameters = new(StringComparer.OrdinalIgnoreCase);
        foreach (var part in parts.Skip(1))
        {
            var pair = part.Split('=', 2, StringSplitOptions.TrimEntries);
            if (pair.Length != 2 || string.IsNullOrWhiteSpace(pair[0]) || !parameters.TryAdd(pair[0], pair[1]))
            {
                parsed = default;
                return false;
            }
        }

        parsed = new ParsedContentType(parts[0], parameters);
        return true;
    }
}

internal sealed class SessionSnapshot(string sessionId, int bufferWindowMs, Dictionary<string, StreamState> streams)
{
    public string SessionId { get; } = sessionId;

    public int BufferWindowMs { get; set; } = bufferWindowMs;

    public Dictionary<string, StreamState> Streams { get; } = streams;

    public DateTimeOffset? DisconnectedAt { get; set; }
}

internal sealed class StreamState(string id, string contentType, CulpeoStreamType type, string? purpose, long currentOffset, OffsetType offsetType)
{
    private readonly List<OffsetCheckpoint> checkpoints = [];
    private readonly int? pcmStride = offsetType == OffsetType.Time ? GetRequiredPcmStride(contentType) : null;

    public string Id { get; } = id;

    public string ContentType { get; } = contentType;

    public CulpeoStreamType Type { get; } = type;

    public string? Purpose { get; } = purpose;

    public OffsetType OffsetType { get; } = offsetType;

    public long CurrentOffset { get; private set; } = currentOffset;

    public long ConfirmedResumeOffset { get; set; }

    public void RecordFrame(long offset, DateTimeOffset timestamp)
    {
        checkpoints.Add(new OffsetCheckpoint(offset, timestamp));
    }

    public long GetEarliestAvailableOffset(TimeSpan window, DateTimeOffset now)
    {
        var cutoff = now - window;
        checkpoints.RemoveAll(checkpoint => checkpoint.RecordedAt < cutoff);
        return checkpoints.Count == 0 ? CurrentOffset : checkpoints[0].Offset;
    }

    public void AdvanceOffset(int payloadLength)
    {
        CurrentOffset += offsetType switch
        {
            OffsetType.Time => GetPcmIncrement(payloadLength, pcmStride!.Value),
            OffsetType.Byte => payloadLength,
            OffsetType.Message => 1,
            _ => throw new ArgumentOutOfRangeException(nameof(offsetType), $"Unknown offset type: {offsetType}")
        };
    }

    private static int GetRequiredPcmStride(string contentType)
    {
        if (!ContentTypeUtilities.TryParseContentType(contentType, out var parsed)
            || !string.Equals(parsed.MediaType, "audio/pcm", StringComparison.OrdinalIgnoreCase))
        {
            throw new ProtocolValidationException("protocol-error", "Offset type 'time' requires an audio/pcm content type with valid rate, channels, and bits parameters.");
        }

        var channels = RequirePcmParam(parsed, "channels", v => v >= 1);
        var bits = RequirePcmParam(parsed, "bits", v => v > 0 && v % 8 == 0);
        _ = RequirePcmParam(parsed, "rate", v => v > 0); // validated for presence; not used in stride calculation
        return checked(channels * (bits / 8));
    }

    private static int RequirePcmParam(ParsedContentType parsed, string name, Func<int, bool> predicate)
    {
        if (!parsed.Parameters.TryGetValue(name, out var raw)
            || !int.TryParse(raw, NumberStyles.Integer, CultureInfo.InvariantCulture, out var value)
            || !predicate(value))
        {
            throw new ProtocolValidationException("protocol-error", $"PCM streams must declare a valid '{name}' parameter.");
        }

        return value;
    }

    private static int GetPcmIncrement(int payloadLength, int stride)
    {
        if (payloadLength % stride != 0)
        {
            throw new ProtocolValidationException("protocol-error", "PCM payload length must align with the declared sample format.");
        }

        return payloadLength / stride;
    }
}

internal readonly record struct OffsetCheckpoint(long Offset, DateTimeOffset RecordedAt);

internal sealed record StreamDeclaration(string ContentType, CulpeoStreamType Type, string? Purpose, string? IdHint, long? ResumeOffset, OffsetType OffsetType);

internal sealed record InitRequest(string Version, IReadOnlyList<StreamDeclaration> Streams)
{
    public bool IsResumption => Streams.Any(stream => stream.ResumeOffset.HasValue || !string.IsNullOrWhiteSpace(stream.IdHint));
}

internal sealed class ProtocolValidationException(string code, string reason, ReadOnlyMemory<byte>? body = null) : Exception(reason)
{
    public string Code { get; } = code;

    public string Reason { get; } = reason;

    public ReadOnlyMemory<byte>? Body { get; } = body;
}
