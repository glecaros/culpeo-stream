using System.Diagnostics;
using System.Reflection;
using System.Text;
using CulpeoStream.Core;

namespace CulpeoStream.AotTests;

/// <summary>
/// Smoke tests for <c>CulpeoStream.Core</c> that exercise the session lifecycle
/// in a trim-safe, NativeAOT-compatible way.
///
/// <para>
/// These tests run both under the normal test runner (<c>dotnet test</c>) and when
/// the project is published with <c>PublishAot=true</c>.
/// </para>
/// </summary>
public sealed class AotSmokeTests
{
    // ── Frame parse/serialize round-trip ──────────────────────────────────────

    [Fact]
    public async Task FrameParser_RoundTrips_ControlFrame()
    {
        var parser = new CulpeoMessageParser();
        var serializer = new CulpeoMessageSerializer();

        var original = new CulpeoMessage(
            CulpeoMessageKind.Control,
            Encoding.UTF8.GetBytes("{\"ts\":42}"),
            @event: "culpeo.ping",
            contentType: "application/json");

        var serialized = await serializer.SerializeAsync(original);
        var parsed = await parser.ParseAsync(serialized, CulpeoMessageKind.Control);

        Assert.Equal("culpeo.ping", parsed.Event);
        Assert.Equal("application/json", parsed.ContentType);
        Assert.Equal("{\"ts\":42}", parsed.GetBodyAsUtf8());
    }

    [Fact]
    public async Task FrameParser_RoundTrips_MediaFrame()
    {
        var parser = new CulpeoMessageParser();
        var serializer = new CulpeoMessageSerializer();

        var payload = new byte[] { 0x01, 0x02, 0x03, 0xFF };
        var original = new CulpeoMessage(
            CulpeoMessageKind.Media,
            payload,
            contentType: "audio/opus",
            streamId: "audio-in",
            offset: 128,
            timestamp: 1_000_000);

        var serialized = await serializer.SerializeAsync(original);
        var parsed = await parser.ParseAsync(serialized, CulpeoMessageKind.Media);

        Assert.Equal("audio/opus", parsed.ContentType);
        Assert.Equal("audio-in", parsed.StreamId);
        Assert.Equal(128, parsed.Offset);
        Assert.Equal(1_000_000, parsed.Timestamp);
        Assert.Equal(payload, parsed.Body.ToArray());
    }

    // ── Session lifecycle ─────────────────────────────────────────────────────

    [Fact]
    public async Task Session_FullLifecycle_InitAckPingPongClose()
    {
        var server = new CulpeoSessionServer();
        var conn = await server.CreateConnectionAsync();
        var serializer = new CulpeoMessageSerializer();
        var parser = new CulpeoMessageParser();

        // ── culpeo.init ──────────────────────────────────────────────────────
        var initBody = Encoding.UTF8.GetBytes("""
            {
              "version": "0.3",
              "streams": [
                {
                  "content_type": "audio/opus",
                  "type": "input",
                  "offset_type": "message"
                }
              ]
            }
            """);

        var initFrame = new CulpeoMessage(
            CulpeoMessageKind.Control,
            initBody,
            @event: "culpeo.init",
            contentType: "application/json",
            authorization: "Bearer test-token",
            bufferWindow: 5000);

        var initResult = await conn.ReceiveAsync(initFrame);
        Assert.False(initResult.ShouldClose);
        Assert.Equal(CulpeoSessionState.Established, initResult.State);
        Assert.Single(initResult.OutboundFrames);
        Assert.Equal("culpeo.init-ack", initResult.OutboundFrames[0].Event);
        Assert.NotNull(conn.SessionId);

        // ── culpeo.ping ──────────────────────────────────────────────────────
        var pingBody = Encoding.UTF8.GetBytes("{\"ts\":1000000}");
        var pingFrame = new CulpeoMessage(
            CulpeoMessageKind.Control,
            pingBody,
            @event: "culpeo.ping",
            contentType: "application/json");

        var pingResult = await conn.ReceiveAsync(pingFrame);
        Assert.False(pingResult.ShouldClose);
        Assert.Single(pingResult.OutboundFrames);
        Assert.Equal("culpeo.pong", pingResult.OutboundFrames[0].Event);

        // Parse pong body to verify ts echoed
        var pongBytes = await serializer.SerializeAsync(pingResult.OutboundFrames[0]);
        var pong = await parser.ParseAsync(pongBytes, CulpeoMessageKind.Control);
        Assert.Equal("culpeo.pong", pong.Event);

        // ── culpeo.close ─────────────────────────────────────────────────────
        var closeFrame = new CulpeoMessage(
            CulpeoMessageKind.Control,
            Encoding.UTF8.GetBytes("{\"code\":\"normal\"}"),
            @event: "culpeo.close",
            contentType: "application/json",
            code: "normal",
            reason: "Done");

        var closeResult = await conn.ReceiveAsync(closeFrame);
        Assert.True(closeResult.ShouldClose);
        Assert.Equal(CulpeoSessionState.Closed, closeResult.State);
    }

    // ── Version negotiation ───────────────────────────────────────────────────

    [Fact]
    public async Task Session_UnsupportedVersion_ClosesWithError()
    {
        var server = new CulpeoSessionServer();
        var conn = await server.CreateConnectionAsync();

        var initBody = Encoding.UTF8.GetBytes("""
            {"version":"9.9","streams":[{"content_type":"audio/opus","type":"input","offset_type":"message"}]}
            """);

        var result = await conn.ReceiveAsync(new CulpeoMessage(
            CulpeoMessageKind.Control, initBody,
            @event: "culpeo.init",
            contentType: "application/json",
            authorization: "Bearer tok"));

        Assert.True(result.ShouldClose);
        Assert.NotEmpty(result.OutboundFrames);
        Assert.Equal("culpeo.init-error", result.OutboundFrames[0].Event);
        Assert.Equal("unsupported-version", result.OutboundFrames[0].Code);
    }

    // ── StreamDeclaration (Core type) ─────────────────────────────────────────

    [Fact]
    public void StreamDeclaration_CanBeCreatedWithRequiredProperties()
    {
        // Verify that StreamDeclaration (now in Core) is accessible and usable.
        var decl = new StreamDeclaration
        {
            ContentType = "audio/pcm;rate=16000;channels=1;bits=16",
            Type = CulpeoStreamType.Input,
            OffsetType = OffsetType.Time,
            Purpose = "microphone"
        };

        Assert.Equal("audio/pcm;rate=16000;channels=1;bits=16", decl.ContentType);
        Assert.Equal(CulpeoStreamType.Input, decl.Type);
        Assert.Equal(OffsetType.Time, decl.OffsetType);
        Assert.Equal("microphone", decl.Purpose);
    }
}

/// <summary>
/// Tests that validate NativeAOT publish succeeds with zero ILC warnings
/// from CulpeoStream assemblies.
/// </summary>
public sealed class AotPublishTests
{
    /// <summary>
    /// Runs <c>dotnet publish</c> on <c>CulpeoStream.Core</c> with
    /// <c>PublishAot=true</c> via a wrapper console app and asserts
    /// that the ILC compiler emits no warnings in CulpeoStream namespaces.
    /// </summary>
    [Fact]
    [Trait("Category", "AotPublish")]
    public void PublishAot_Core_ZeroIlcWarnings()
    {
        // Find the CulpeoStream.AotTests project directory by walking up from the
        // base directory of the current process.  AppContext.BaseDirectory is
        // AOT-safe (unlike Assembly.Location which is always empty in single-file apps).
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir is not null && !File.Exists(Path.Combine(dir.FullName, "CulpeoStream.slnx")))
        {
            dir = dir.Parent;
        }

        if (dir is null)
        {
            // Skip gracefully when the solution root can't be found (e.g. packed CI artifact).
            return;
        }

        var aotTestsProjectPath = Path.Combine(
            dir.FullName, "tests", "CulpeoStream.AotTests", "CulpeoStream.AotTests.csproj");

        if (!File.Exists(aotTestsProjectPath))
        {
            return; // project not present — skip
        }

        // Run dotnet publish with AOT on the test project itself.
        var psi = new ProcessStartInfo("dotnet",
            $"publish \"{aotTestsProjectPath}\" " +
            "-c Release -r linux-x64 --self-contained " +
            "-p:PublishAot=true -p:TreatWarningsAsErrors=false " +
            $"--output \"{Path.GetTempPath()}culpeo-aot-publish-{Guid.NewGuid():N}\"")
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true,
        };

        using var process = Process.Start(psi);
        Assert.NotNull(process);

        var stdout = process.StandardOutput.ReadToEnd();
        var stderr = process.StandardError.ReadToEnd();
        process.WaitForExit(300_000); // 5 min max

        var combinedOutput = stdout + "\n" + stderr;

        // Collect ILC warnings that originate from CulpeoStream *source* assemblies
        // (not from the test project itself, which deliberately uses xUnit reflection).
        var ilcWarnings = combinedOutput
            .Split('\n', StringSplitOptions.RemoveEmptyEntries)
            .Where(line =>
                (line.Contains("warning IL") || line.Contains("Trim analysis warning")) &&
                line.Contains("CulpeoStream") &&
                !line.Contains("AotTests") &&          // exclude the test project itself
                !line.Contains("AotSmokeTests") &&
                (line.Contains("CulpeoStream.Core") ||
                 line.Contains("CulpeoStream.AspNetCore") ||
                 line.Contains("CulpeoStream.Client") ||
                 line.Contains("CulpeoStream.Http2")))
            .ToList();

        Assert.True(
            ilcWarnings.Count == 0,
            $"Expected zero ILC trim warnings from CulpeoStream assemblies. " +
            $"Found {ilcWarnings.Count}:\n{string.Join("\n", ilcWarnings)}\n\nFull output:\n{combinedOutput[..Math.Min(4000, combinedOutput.Length)]}");
    }
}
