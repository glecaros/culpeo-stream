using System.Net;
using System.Net.WebSockets;
using CulpeoStream.AspNetCore.Internal;
using CulpeoStream.Core;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using Microsoft.Net.Http.Headers;

namespace CulpeoStream.AspNetCore;

/// <summary>
/// ASP.NET Core middleware that upgrades HTTP connections to CulpeoStream sessions
/// over WebSocket. Enforces the WebSocket sub-protocol, TLS requirements,
/// per-IP rate limiting, and delegates session handling to an
/// <see cref="ICulpeoStreamHandler"/>.
/// </summary>
public sealed class CulpeoStreamMiddleware
{
    private readonly RequestDelegate _next;
    private readonly ICulpeoStreamHandler _handler;
    private readonly CulpeoSessionServer _sessionServer;
    private readonly IpRateLimiter _rateLimiter;
    private readonly CulpeoStreamOptions _options;
    private readonly IHostEnvironment _environment;
    private readonly ILogger<CulpeoStreamMiddleware> _logger;

    // WebSocket sub-protocol required by §B.1.
    private const string RequiredSubProtocol = "culpeostream";

    public CulpeoStreamMiddleware(
        RequestDelegate next,
        ICulpeoStreamHandler handler,
        CulpeoSessionServer sessionServer,
        IOptions<CulpeoStreamOptions> options,
        IHostEnvironment environment,
        ILogger<CulpeoStreamMiddleware> logger,
        IServiceProvider services)
    {
        _next = next;
        _handler = handler;
        _sessionServer = sessionServer;
        _rateLimiter = services.GetRequiredService<IpRateLimiter>();
        _options = options.Value;
        _environment = environment;
        _logger = logger;
    }

    public async Task InvokeAsync(HttpContext context)
    {
        if (!context.WebSockets.IsWebSocketRequest)
        {
            await _next(context).ConfigureAwait(false);
            return;
        }

        // ── Enforce wss:// in production (§3.1, §B.5) ──────────────────────
        if (_options.RequireEncryptedTransport && _environment.IsProduction())
        {
            if (!IsSecureRequest(context))
            {
                _logger.LogWarning(
                    "Rejected insecure WebSocket connection from {RemoteIp}.",
                    context.Connection.RemoteIpAddress);

                context.Response.StatusCode = StatusCodes.Status403Forbidden;
                await context.Response.WriteAsync(
                    "CulpeoStream requires wss:// in production.", context.RequestAborted)
                    .ConfigureAwait(false);
                return;
            }
        }

        // ── Validate sub-protocol (§B.1) ────────────────────────────────────
        if (!RequestedSubProtocols(context).Contains(RequiredSubProtocol, StringComparer.Ordinal))
        {
            _logger.LogWarning(
                "Rejected WebSocket upgrade without 'culpeostream' sub-protocol from {RemoteIp}.",
                context.Connection.RemoteIpAddress);

            context.Response.StatusCode = StatusCodes.Status400BadRequest;
            await context.Response.WriteAsync(
                "Missing required WebSocket sub-protocol 'culpeostream'.", context.RequestAborted)
                .ConfigureAwait(false);
            return;
        }

        // ── Per-IP rate limiting (§A.5) ─────────────────────────────────────
        var ip = GetClientIp(context);
        if (!_rateLimiter.TryAcquire(ip, _options.MaxConnectionsPerIpPerMinute))
        {
            _logger.LogWarning(
                "Rate limit exceeded for IP {RemoteIp} ({Max}/min).",
                ip, _options.MaxConnectionsPerIpPerMinute);

            context.Response.StatusCode = StatusCodes.Status429TooManyRequests;
            context.Response.Headers[HeaderNames.RetryAfter] = "60";
            await context.Response.WriteAsync("Rate limit exceeded.", context.RequestAborted)
                .ConfigureAwait(false);
            return;
        }

        // ── Accept WebSocket upgrade ─────────────────────────────────────────
        using var ws = await context.WebSockets
            .AcceptWebSocketAsync(RequiredSubProtocol)
            .ConfigureAwait(false);

        _logger.LogDebug("WebSocket connection accepted from {RemoteIp}.", ip);

        // ── Run CulpeoStream session ─────────────────────────────────────────
        await RunSessionAsync(ws, context.RequestAborted).ConfigureAwait(false);
    }

    // ── Internal helpers ──────────────────────────────────────────────────────

    /// <summary>
    /// Runs the full CulpeoStream protocol loop for a single WebSocket connection.
    /// </summary>
    internal async Task RunSessionAsync(WebSocket ws, CancellationToken cancellationToken)
    {
        var connection = await _sessionServer.CreateConnectionAsync(cancellationToken)
            .ConfigureAwait(false);

        var logger = _logger;
        var adapter = new WebSocketTransportAdapter(ws, connection, _handler, _options, logger);

        await adapter.RunAsync(cancellationToken).ConfigureAwait(false);
    }

    // ── Request validation helpers ────────────────────────────────────────────

    private bool IsSecureRequest(HttpContext context)
    {
        // Check X-Forwarded-Proto first when behind a trusted proxy
        if (_options.TrustForwardedProto)
        {
            var forwarded = context.Request.Headers["X-Forwarded-Proto"].FirstOrDefault();
            if (!string.IsNullOrEmpty(forwarded))
            {
                return string.Equals(forwarded, "https", StringComparison.OrdinalIgnoreCase);
            }
        }

        return context.Request.IsHttps;
    }

    private static IEnumerable<string> RequestedSubProtocols(HttpContext context)
    {
        // Sec-WebSocket-Protocol may be comma-separated (RFC 6455 §4.1)
        var header = context.Request.Headers[HeaderNames.SecWebSocketProtocol];
        foreach (var entry in header)
        {
            if (entry is null) continue;
            foreach (var part in entry.Split(','))
            {
                yield return part.Trim();
            }
        }
    }

    private string GetClientIp(HttpContext context)
    {
        if (_options.TrustedProxyCount > 0)
        {
            var forwarded = context.Request.Headers["X-Forwarded-For"].FirstOrDefault();
            if (!string.IsNullOrEmpty(forwarded))
            {
                var ips = forwarded.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
                var index = ips.Length - _options.TrustedProxyCount;
                if (index >= 0 && index < ips.Length)
                {
                    return ips[index];
                }
            }
        }

        return context.Connection.RemoteIpAddress?.ToString() ?? "unknown";
    }
}
