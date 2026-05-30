using CulpeoStream.AspNetCore.Internal;
using CulpeoStream.Core;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.DependencyInjection.Extensions;
using Microsoft.Extensions.Options;

namespace CulpeoStream.AspNetCore;

/// <summary>
/// Extension methods for registering CulpeoStream services in the DI container.
/// </summary>
public static class ServiceCollectionExtensions
{
    /// <summary>
    /// Adds the CulpeoStream infrastructure services (session server, rate limiter,
    /// and options) to the service collection.
    /// </summary>
    /// <remarks>
    /// Does NOT register an <see cref="ICulpeoStreamHandler"/>. Specify the handler
    /// when calling <c>app.MapCulpeoStream()</c> or
    /// <c>app.UseMiddleware&lt;CulpeoStreamMiddleware&gt;(handler)</c>.
    /// </remarks>
    public static IServiceCollection AddCulpeoStream(
        this IServiceCollection services,
        Action<CulpeoStreamOptions>? configure = null)
    {
        ArgumentNullException.ThrowIfNull(services);

        // Register options
        var optBuilder = services.AddOptions<CulpeoStreamOptions>();
        if (configure is not null)
        {
            optBuilder.Configure(configure);
        }

        // Register the Core session server as a singleton.
        // The server is constructed from CulpeoStreamOptions after DI builds.
        services.TryAddSingleton(sp =>
        {
            var options = sp.GetRequiredService<IOptions<CulpeoStreamOptions>>().Value;
            ValidateOptions(options);

            var coreOptions = new CulpeoSessionOptions
            {
                SupportedVersions = options.SupportedVersions,
                MaxBufferWindowMs = options.MaxBufferWindowMs,
                MaxStreamCount = options.MaxStreamsPerSession,
                AuthChallengeTimeout = options.AuthChallengeTimeout,
                MinAuthRefreshIntervalSeconds = options.MinAuthRefreshIntervalSeconds
            };

            return new CulpeoSessionServer(coreOptions);
        });

        // Per-IP rate limiter (single in-memory instance)
        services.TryAddSingleton<IpRateLimiter>();

        return services;
    }

    private static void ValidateOptions(CulpeoStreamOptions options)
    {
        if (options.SupportedVersions is null || options.SupportedVersions.Count == 0)
        {
            throw new InvalidOperationException(
                "CulpeoStreamOptions.SupportedVersions must contain at least one version.");
        }

        if (options.MaxBufferWindowMs <= 0)
        {
            throw new InvalidOperationException(
                "CulpeoStreamOptions.MaxBufferWindowMs must be greater than zero.");
        }

        if (options.MaxStreamsPerSession <= 0)
        {
            throw new InvalidOperationException(
                "CulpeoStreamOptions.MaxStreamsPerSession must be greater than zero.");
        }

        if (options.AuthChallengeTimeout <= TimeSpan.Zero)
        {
            throw new InvalidOperationException(
                "CulpeoStreamOptions.AuthChallengeTimeout must be greater than zero. " +
                "The timeout may not be disabled.");
        }
    }
}
