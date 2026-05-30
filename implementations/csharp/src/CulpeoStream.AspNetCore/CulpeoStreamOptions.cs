namespace CulpeoStream.AspNetCore;

/// <summary>
/// Configuration options for the CulpeoStream ASP.NET Core integration.
/// </summary>
public sealed class CulpeoStreamOptions
{
    // ── Protocol options (forwarded to CulpeoStream.Core) ─────────────────────

    /// <summary>Protocol versions the server will accept.</summary>
    public IReadOnlyList<string> SupportedVersions { get; set; } = ["0.3"];

    /// <summary>
    /// Maximum allowed Buffer-Window in milliseconds. Client requests above
    /// this value are clamped. Default is 30 000 ms (30 s) per §7.4.1.
    /// </summary>
    public int MaxBufferWindowMs { get; set; } = 30_000;

    /// <summary>
    /// Maximum number of streams per session. Default is 16 per §5.6.
    /// </summary>
    public int MaxStreamsPerSession { get; set; } = 16;

    /// <summary>
    /// How long the server waits for a <c>culpeo.auth-response</c> before
    /// closing the session with <c>auth-expired</c>. Default is 30 s per §A.4.
    /// Must not be disabled (zero is rejected).
    /// </summary>
    public TimeSpan AuthChallengeTimeout { get; set; } = TimeSpan.FromSeconds(30);

    // ── ASP.NET Core–specific options ─────────────────────────────────────────

    /// <summary>
    /// Session idle timeout. Sessions with no traffic for this duration are
    /// closed with code <c>idle-timeout</c>.
    /// </summary>
    public TimeSpan IdleTimeout { get; set; } = TimeSpan.FromMinutes(5);

    /// <summary>
    /// Maximum new WebSocket connections accepted per IP address per minute.
    /// Default is 10. Set to 0 to disable rate limiting (not recommended).
    /// </summary>
    public int MaxConnectionsPerIpPerMinute { get; set; } = 10;

    /// <summary>
    /// When <see langword="true"/> (the default), the middleware rejects plain
    /// <c>ws://</c> connections in a production environment. In development the
    /// check is skipped. See §3.1 and §B.5.
    /// </summary>
    public bool RequireEncryptedTransport { get; set; } = true;

    /// <summary>
    /// When <see langword="true"/> (the default), the middleware trusts the
    /// <c>X-Forwarded-Proto</c> header when evaluating whether the original
    /// request arrived over TLS. Enable only when a trusted reverse proxy
    /// terminates TLS in front of this server.
    /// </summary>
    public bool TrustForwardedProto { get; set; } = true;

    /// <summary>
    /// Number of trusted reverse-proxy hops between the Internet and this server.
    /// When greater than zero and an <c>X-Forwarded-For</c> header is present, the
    /// middleware extracts the real client IP by skipping this many entries from the
    /// right of the header (those are the trusted proxy addresses), and uses the
    /// next entry as the client IP for rate-limiting purposes.
    ///
    /// Security model: only set this to the exact number of proxies you control.
    /// Setting it too high lets attackers spoof arbitrary IPs; setting it too low
    /// (or leaving it at the default of 0) causes all clients behind a proxy to
    /// share a single rate-limit bucket.
    ///
    /// Default is 0 (disabled; <c>RemoteIpAddress</c> is used as-is).
    /// When <see cref="TrustForwardedProto"/> is <see langword="true"/>, consider
    /// setting this to <c>1</c>.
    /// </summary>
    public int TrustedProxyCount { get; set; } = 0;

    /// <summary>
    /// Maximum permitted WebSocket message size in bytes. Fragments are accumulated
    /// until this limit is reached; if a message would exceed it the connection is
    /// closed with <c>protocol-error</c>. Default is 1 MiB.
    /// </summary>
    public int MaxMessageBytes { get; set; } = 1 * 1024 * 1024;

    /// <summary>
    /// Minimum number of seconds that must elapse between successive
    /// <c>culpeo.auth-refresh</c> challenges on the same session. Default is 30 s.
    /// </summary>
    public int MinAuthRefreshIntervalSeconds { get; set; } = 30;
}
