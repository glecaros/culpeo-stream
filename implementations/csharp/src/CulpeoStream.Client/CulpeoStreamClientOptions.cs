namespace CulpeoStream.Client;

/// <summary>
/// Configuration for <see cref="CulpeoStreamClient"/>.
/// </summary>
public sealed class CulpeoStreamClientOptions
{
    /// <summary>
    /// Bearer token (or full <c>Authorization</c> header value) used for the initial <c>culpeo.init</c>.
    /// Never logged or exposed in traces.
    /// </summary>
    public required string Authorization { get; init; }

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
    /// Called when the server sends <c>culpeo.auth-refresh</c>. Must return a fresh token
    /// (full <c>Authorization</c> header value, e.g. <c>"Bearer eyJ…"</c>).
    /// If <see langword="null"/>, the client closes with <c>auth-error</c> on any auth-refresh challenge.
    /// </summary>
    public Func<CancellationToken, Task<string>>? GetToken { get; init; }

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
