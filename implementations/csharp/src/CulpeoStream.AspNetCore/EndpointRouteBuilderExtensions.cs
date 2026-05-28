using CulpeoStream.AspNetCore.Internal;
using CulpeoStream.Core;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Routing;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;

namespace CulpeoStream.AspNetCore;

/// <summary>
/// Extension methods for mapping CulpeoStream endpoints in the ASP.NET Core
/// endpoint routing pipeline.
/// </summary>
public static class EndpointRouteBuilderExtensions
{
    /// <summary>
    /// Maps a CulpeoStream WebSocket endpoint at the specified route pattern.
    /// </summary>
    /// <param name="endpoints">The endpoint route builder.</param>
    /// <param name="pattern">The URL pattern, e.g. <c>/ws</c>.</param>
    /// <param name="handler">
    /// The application handler that will receive session lifecycle events.
    /// </param>
    /// <returns>A builder to further configure the endpoint.</returns>
    /// <example>
    /// <code>
    /// app.MapCulpeoStream("/ws", new MyAudioSessionHandler());
    /// </code>
    /// </example>
    public static IEndpointConventionBuilder MapCulpeoStream(
        this IEndpointRouteBuilder endpoints,
        string pattern,
        ICulpeoStreamHandler handler)
    {
        ArgumentNullException.ThrowIfNull(endpoints);
        ArgumentNullException.ThrowIfNull(pattern);
        ArgumentNullException.ThrowIfNull(handler);

        var app = endpoints.CreateApplicationBuilder();

        // Insert WebSocket middleware into the sub-pipeline so that
        // context.WebSockets.IsWebSocketRequest works correctly.
        app.UseWebSockets();

        app.Run(async context =>
        {
            if (!context.WebSockets.IsWebSocketRequest)
            {
                context.Response.StatusCode = StatusCodes.Status400BadRequest;
                await context.Response.WriteAsync(
                    "This endpoint requires a WebSocket upgrade.", context.RequestAborted)
                    .ConfigureAwait(false);
                return;
            }

            var sessionServer = context.RequestServices.GetRequiredService<CulpeoSessionServer>();
            var options = context.RequestServices.GetRequiredService<IOptions<CulpeoStreamOptions>>();
            var environment = context.RequestServices.GetRequiredService<IHostEnvironment>();
            var loggerFactory = context.RequestServices.GetRequiredService<ILoggerFactory>();
            var logger = loggerFactory.CreateLogger<CulpeoStreamMiddleware>();

            // Reuse the middleware to handle the connection so the logic is DRY
            var middleware = new CulpeoStreamMiddleware(
                next: _ => Task.CompletedTask, // endpoint – no next middleware
                handler: handler,
                sessionServer: sessionServer,
                options: options,
                environment: environment,
                logger: logger,
                services: context.RequestServices);

            await middleware.InvokeAsync(context).ConfigureAwait(false);
        });

        return endpoints.Map(pattern, app.Build())
            .WithDisplayName($"CulpeoStream: {pattern}");
    }
}
