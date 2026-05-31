namespace CulpeoStream.Client;

/// <summary>
/// Configuration for <see cref="CulpeoStreamClient"/>.
/// </summary>
public sealed class CulpeoStreamClientOptions
{
    /// <summary>
    /// Static bearer token (or full <c>Authorization</c> header value) used when
    /// <see cref="GetToken"/> is <see langword="null"/>.
    /// Never logged or exposed in traces.
    /// <para>
    /// At least one of <see cref="Authorization"/> or <see cref="GetToken"/> must be non-null.
    /// When both are provided, <see cref="GetToken"/> takes precedence on every connect and
    /// reconnect.
    /// </para>
    /// </summary>
    public string? Authorization { get; init; }

    /// <summary>
    /// Streams to declare in <c>culpeo.init</c>.
    /// </summary>
    public required IReadOnlyList<StreamDeclaration> Streams { get; init; }

    /// <summary>
    /// Protocol version to declare. Defaults to <c>0.3</c>.
    /// </summary>
    public string Version { get; init; } = "0.3";

    // ── Reconnection ────────────────────────────────────────────────────────────

    /// <summary>
    /// Whether to automatically reconnect and resume the session on connection loss.
    /// Defaults to <see langword="true"/>.
    /// </summary>
    public bool AutoReconnect { get; init; } = true;

    /// <summary>
    /// Maximum number of reconnection attempts before giving up.
    /// Defaults to 10.
    /// </summary>
    public int MaxReconnectAttempts { get; init; } = 10;

    /// <summary>
    /// Backoff seed: the maximum delay for attempt 0 (before jitter). Doubles each attempt up to <see cref="MaxBackoff"/>.
    /// Defaults to 1 second.
    /// </summary>
    public TimeSpan InitialBackoff { get; init; } = TimeSpan.FromSeconds(1);

    /// <summary>
    /// Maximum backoff cap before jitter. Defaults to 30 seconds.
    /// </summary>
    public TimeSpan MaxBackoff { get; init; } = TimeSpan.FromSeconds(30);

    // ── Auth refresh ────────────────────────────────────────────────────────────

    /// <summary>
    /// Called on every <c>culpeo.init</c> (including reconnects) and when the server sends
    /// <c>culpeo.auth-refresh</c>. Must return a fresh token (full <c>Authorization</c> header
    /// value, e.g. <c>"Bearer eyJ…"</c>).
    /// <para>
    /// When non-null this takes precedence over <see cref="Authorization"/> on every connect and
    /// reconnect, ensuring expired tokens are never reused.
    /// </para>
    /// <para>
    /// If <see langword="null"/> and no <c>auth-refresh</c> challenge arrives, <see cref="Authorization"/>
    /// is used. If <see langword="null"/> and an <c>auth-refresh</c> does arrive, the client
    /// closes with <c>auth-expired</c>.
    /// </para>
    /// </summary>
    public Func<CancellationToken, Task<string>>? GetToken { get; init; }

    // ── Channel ──────────────────────────────────────────────────────────────

    /// <summary>
    /// Capacity of the bounded event channel that buffers frames between the receive loop and
    /// <see cref="CulpeoStreamClient.ReceiveAsync"/>.
    /// Defaults to 1024.
    /// <para>
    /// <b>Important:</b> callers MUST consume <see cref="CulpeoStreamClient.ReceiveAsync"/>
    /// continuously. A full channel will block the receive loop (back-pressure), which can
    /// delay pong responses and session resumption.
    /// </para>
    /// </summary>
    public int EventChannelCapacity { get; init; } = 1024;

    // ── Security ────────────────────────────────────────────────────────────────

    /// <summary>
    /// Allow plain <c>ws://</c> connections. Defaults to <see langword="false"/>.
    /// <para>
    /// <b>Security warning:</b> plain WebSocket connections transmit tokens in clear text.
    /// Only set this to <see langword="true"/> in local development environments (§3.1).
    /// </para>
    /// </summary>
    public bool AllowInsecureConnections { get; init; } = false;

    /// <summary>
    /// Requested <c>Buffer-Window</c> in milliseconds. Defaults to 5000 ms.
    /// </summary>
    public int BufferWindowMs { get; init; } = 5_000;
}
