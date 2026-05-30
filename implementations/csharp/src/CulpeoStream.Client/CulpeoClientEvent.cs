using System.Text.Json;

namespace CulpeoStream.Client;

/// <summary>
/// Base type for all events produced by <see cref="CulpeoStreamClient.ReceiveAsync"/>.
/// </summary>
public abstract record CulpeoClientEvent;

/// <summary>
/// A media frame was received on the given stream.
/// </summary>
/// <param name="StreamId">Server-assigned stream identifier.</param>
/// <param name="Data">Raw media payload.</param>
/// <param name="Offset">Frame offset within the stream (§8.2).</param>
public sealed record MediaReceived(string StreamId, ReadOnlyMemory<byte> Data, long Offset) : CulpeoClientEvent;

/// <summary>
/// An application event frame was received (non-<c>culpeo.</c> namespace).
/// </summary>
/// <param name="EventName">Full event name, e.g. <c>myservice.transcript</c>.</param>
/// <param name="Body">Parsed JSON body of the event.</param>
public sealed record ApplicationEventReceived(string EventName, JsonElement Body) : CulpeoClientEvent;

/// <summary>
/// The session was newly established (first connect or post-reconnect fresh session).
/// </summary>
/// <param name="SessionId">Server-assigned session identifier.</param>
public sealed record SessionEstablished(string SessionId) : CulpeoClientEvent;

/// <summary>
/// The session was successfully resumed after a reconnection.
/// </summary>
/// <param name="SessionId">Server-assigned session identifier (same as original session).</param>
public sealed record SessionResumed(string SessionId) : CulpeoClientEvent;

/// <summary>
/// The session ended. No more events will be produced after this.
/// </summary>
/// <param name="Reason">Human-readable description (close code or transport error).</param>
public sealed record Disconnected(string Reason) : CulpeoClientEvent;
