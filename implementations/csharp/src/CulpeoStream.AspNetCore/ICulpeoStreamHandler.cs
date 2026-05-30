using CulpeoStream.Core;

namespace CulpeoStream.AspNetCore;

/// <summary>
/// Contextual information about an incoming media frame delivered to
/// <see cref="ICulpeoStreamHandler.OnMediaFrameAsync"/>.
/// </summary>
public sealed class CulpeoMediaFrameContext
{
    internal CulpeoMediaFrameContext(
        string streamId,
        long offset,
        long? timestamp,
        string contentType,
        ReadOnlyMemory<byte> payload)
    {
        StreamId = streamId;
        Offset = offset;
        Timestamp = timestamp;
        ContentType = contentType;
        Payload = payload;
    }

    /// <summary>Server-assigned stream identifier.</summary>
    public string StreamId { get; }

    /// <summary>
    /// Frame sequence offset within the stream. Increments by sample count for
    /// PCM streams, or by 1 for encoded streams (§8.2).
    /// </summary>
    public long Offset { get; }

    /// <summary>
    /// Presentation timestamp in microseconds since session start, or
    /// <see langword="null"/> if the frame did not include a timestamp.
    /// </summary>
    public long? Timestamp { get; }

    /// <summary>Declared content type for the stream.</summary>
    public string ContentType { get; }

    /// <summary>Raw media payload bytes.</summary>
    public ReadOnlyMemory<byte> Payload { get; }
}

/// <summary>
/// Contextual information about an incoming application event delivered to
/// <see cref="ICulpeoStreamHandler.OnEventAsync"/>.
/// </summary>
public sealed class CulpeoEventContext
{
    internal CulpeoEventContext(string eventName, string? streamId, string jsonBody)
    {
        EventName = eventName;
        StreamId = streamId;
        JsonBody = jsonBody;
    }

    /// <summary>Full event name, e.g. <c>myservice.transcript</c>.</summary>
    public string EventName { get; }

    /// <summary>
    /// Stream-scoped event: the associated stream identifier, or
    /// <see langword="null"/> for session-scoped events (§9.2).
    /// </summary>
    public string? StreamId { get; }

    /// <summary>UTF-8 JSON body of the event frame.</summary>
    public string JsonBody { get; }
}

/// <summary>
/// Represents an established CulpeoStream session as seen by application code.
/// Obtained via <see cref="ICulpeoStreamHandler.OnConnectedAsync"/>.
/// </summary>
public interface ICulpeoStreamSession
{
    /// <summary>Server-assigned session identifier.</summary>
    string SessionId { get; }

    /// <summary>Confirmed streams for this session.</summary>
    IReadOnlyList<CulpeoStreamInfo> Streams { get; }

    /// <summary>
    /// Sends a media frame to the client on the specified output or duplex stream.
    /// The timestamp is computed automatically as microseconds since session start.
    /// </summary>
    /// <exception cref="InvalidOperationException">
    /// Thrown if the stream does not exist, is not an output or duplex stream, or
    /// the session is not established.
    /// </exception>
    Task SendMediaAsync(
        string streamId,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken = default);

    /// <summary>
    /// Sends an application event frame to the client.
    /// </summary>
    /// <param name="eventName">
    /// Namespaced event name, e.g. <c>myservice.transcript</c>. Must not use
    /// the reserved <c>culpeo.</c> prefix.
    /// </param>
    /// <param name="streamId">
    /// Optional stream identifier for stream-scoped events (§9.2).
    /// </param>
    /// <param name="jsonBody">UTF-8 JSON body of the event. Defaults to <c>{}</c>.</param>
    Task SendEventAsync(
        string eventName,
        string? streamId = null,
        string jsonBody = "{}",
        CancellationToken cancellationToken = default);

    /// <summary>Initiates graceful session termination.</summary>
    Task CloseAsync(
        string code = "normal",
        string reason = "Session closed",
        CancellationToken cancellationToken = default);
}

/// <summary>
/// Implement this interface to handle CulpeoStream sessions in your application.
/// Register it via <see cref="EndpointRouteBuilderExtensions.MapCulpeoStream"/>.
/// </summary>
public interface ICulpeoStreamHandler
{
    /// <summary>
    /// Called during <c>culpeo.init</c> processing, <em>before</em> the session
    /// is established. Implementations must validate the bearer token (or any other
    /// credential embedded in the <paramref name="authorization"/> header value).
    /// </summary>
    /// <param name="authorization">
    /// The raw value of the <c>Authorization</c> header from the
    /// <c>culpeo.init</c> frame, e.g. <c>"Bearer eyJhbGci…"</c>. May be empty if
    /// the client omitted the header (the middleware will already have rejected
    /// empty-authorization frames before this call).
    /// </param>
    /// <returns>
    /// <see langword="true"/> to allow the session to be established;
    /// <see langword="false"/> to reject it with <c>culpeo.init-error</c>
    /// code <c>unauthorized</c>.
    /// </returns>
    Task<bool> AuthenticateAsync(string authorization, CancellationToken cancellationToken);

    /// <summary>
    /// Called once after the session is established and <c>culpeo.init-ack</c>
    /// has been sent to the client. Use this to set up any session-level state.
    /// </summary>
    Task OnConnectedAsync(ICulpeoStreamSession session, CancellationToken cancellationToken);

    /// <summary>
    /// Called for each incoming media frame on an <c>input</c> or <c>duplex</c>
    /// stream. Not called for media frames the server itself sends.
    /// </summary>
    Task OnMediaFrameAsync(
        ICulpeoStreamSession session,
        CulpeoMediaFrameContext frame,
        CancellationToken cancellationToken);

    /// <summary>
    /// Called for each incoming application event frame (non-<c>culpeo.</c>
    /// namespace). Protocol events are handled transparently by the middleware.
    /// </summary>
    Task OnEventAsync(
        ICulpeoStreamSession session,
        CulpeoEventContext @event,
        CancellationToken cancellationToken);

    /// <summary>
    /// Called once when the session ends, regardless of whether the close was
    /// graceful. <paramref name="closeCode"/> is the CulpeoStream close code
    /// (e.g., <c>normal</c>, <c>idle-timeout</c>, <c>auth-expired</c>) or
    /// <see langword="null"/> for unexpected transport drops.
    /// </summary>
    Task OnDisconnectedAsync(
        ICulpeoStreamSession session,
        string? closeCode,
        CancellationToken cancellationToken);
}
