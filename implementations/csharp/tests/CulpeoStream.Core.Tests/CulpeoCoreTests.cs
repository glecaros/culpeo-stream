using System.Text;
using System.Text.Json;
using CulpeoStream.Core;

namespace CulpeoStream.Core.Tests;

public sealed class CulpeoCoreTests
{
    [Fact]
    public async Task FrameParser_IgnoresUnknownHeaders_AndParsesControlFrame()
    {
        var parser = new CulpeoFrameParser();
        var bytes = Encoding.UTF8.GetBytes("Event: culpeo.ping\r\nX-Future: ignored\r\nContent-Type: application/json\r\n\r\n{\"ts\":1}");

        var frame = await parser.ParseAsync(bytes, CulpeoFrameKind.Control);

        Assert.Equal("culpeo.ping", frame.Event);
        Assert.Equal("application/json", frame.ContentType);
        Assert.Equal("{\"ts\":1}", frame.GetBodyAsUtf8());
    }

    [Fact]
    public async Task FrameParser_RejectsHeaderBlockExceedingMaxSize()
    {
        var parser = new CulpeoFrameParser(new ParseLimits { MaxHeaderBlockSize = 64 });
        var longValue = new string('x', 100);
        var bytes = Encoding.UTF8.GetBytes($"Event: culpeo.ping\r\nX-Big: {longValue}\r\n\r\n{{}}");

        var ex = await Assert.ThrowsAsync<FormatException>(() => parser.ParseAsync(bytes, CulpeoFrameKind.Control).AsTask());
        Assert.Contains("maximum size", ex.Message);
    }

    [Fact]
    public async Task FrameParser_RejectsExcessiveHeaderCount()
    {
        var parser = new CulpeoFrameParser(new ParseLimits { MaxHeaderCount = 3 });
        StringBuilder sb = new();
        for (var i = 0; i < 5; i++)
        {
            sb.Append($"X-H{i}: val{i}\r\n");
        }
        sb.Append("\r\n{}");
        var bytes = Encoding.UTF8.GetBytes(sb.ToString());

        var ex = await Assert.ThrowsAsync<FormatException>(() => parser.ParseAsync(bytes, CulpeoFrameKind.Control).AsTask());
        Assert.Contains("header count", ex.Message);
    }

    [Fact]
    public async Task FrameParser_RejectsOversizedHeaderName()
    {
        var parser = new CulpeoFrameParser(new ParseLimits { MaxHeaderNameLength = 10 });
        var longName = new string('A', 20);
        var bytes = Encoding.UTF8.GetBytes($"{longName}: value\r\n\r\n{{}}");

        var ex = await Assert.ThrowsAsync<FormatException>(() => parser.ParseAsync(bytes, CulpeoFrameKind.Control).AsTask());
        Assert.Contains("name exceeds", ex.Message);
    }

    [Fact]
    public async Task FrameParser_RejectsOversizedHeaderValue()
    {
        var parser = new CulpeoFrameParser(new ParseLimits { MaxHeaderValueLength = 10 });
        var longValue = new string('v', 20);
        var bytes = Encoding.UTF8.GetBytes($"Event: {longValue}\r\n\r\n{{}}");

        var ex = await Assert.ThrowsAsync<FormatException>(() => parser.ParseAsync(bytes, CulpeoFrameKind.Control).AsTask());
        Assert.Contains("value exceeds", ex.Message);
    }

    [Fact]
    public async Task FrameParser_RejectsForbiddenBytesInHeaderName()
    {
        var parser = new CulpeoFrameParser();
        byte[] bytes = [(byte)'E', 0x00, (byte)'v', (byte)':', (byte)' ', (byte)'x', (byte)'\r', (byte)'\n', (byte)'\r', (byte)'\n'];

        var ex = await Assert.ThrowsAsync<FormatException>(() => parser.ParseAsync(bytes, CulpeoFrameKind.Control).AsTask());
        Assert.Contains("forbidden byte", ex.Message);
    }

    [Fact]
    public async Task FrameParser_RejectsForbiddenBytesInHeaderValue()
    {
        var parser = new CulpeoFrameParser();
        // Build "Event: val\0ue\r\n\r\n"
        List<byte> raw = [];
        raw.AddRange(Encoding.UTF8.GetBytes("Event: val"));
        raw.Add(0x00);
        raw.AddRange(Encoding.UTF8.GetBytes("ue\r\n\r\n"));
        var bytes = raw.ToArray();

        var ex = await Assert.ThrowsAsync<FormatException>(() => parser.ParseAsync(bytes, CulpeoFrameKind.Control).AsTask());
        Assert.Contains("forbidden byte", ex.Message);
    }

    [Fact]
    public async Task FrameParser_RejectsDuplicateReservedHeaders()
    {
        var parser = new CulpeoFrameParser();
        var bytes = Encoding.UTF8.GetBytes("Event: culpeo.ping\r\nEvent: culpeo.pong\r\n\r\n{}");

        var ex = await Assert.ThrowsAsync<FormatException>(() => parser.ParseAsync(bytes, CulpeoFrameKind.Control).AsTask());
        Assert.Contains("Duplicate", ex.Message);
    }

    [Fact]
    public async Task FrameParser_AllowsDuplicateUnknownHeaders()
    {
        var parser = new CulpeoFrameParser();
        var bytes = Encoding.UTF8.GetBytes("Event: culpeo.ping\r\nX-Custom: a\r\nX-Custom: b\r\n\r\n{}");

        var frame = await parser.ParseAsync(bytes, CulpeoFrameKind.Control);
        Assert.Equal("culpeo.ping", frame.Event);
    }

    [Fact]
    public async Task FrameSerializer_RoundTripsControlAndMediaFrames()
    {
        var parser = new CulpeoFrameParser();
        var serializer = new CulpeoFrameSerializer();

        var control = new CulpeoFrame(
            CulpeoFrameKind.Control,
            Encoding.UTF8.GetBytes("{\"version\":\"0.3\"}"),
            @event: "culpeo.init",
            contentType: "application/json",
            authorization: "Bearer hidden",
            bufferWindow: 1000);

        var controlRoundTrip = await parser.ParseAsync(await serializer.SerializeAsync(control), CulpeoFrameKind.Control);
        Assert.Equal(control.Event, controlRoundTrip.Event);
        Assert.Equal(control.ContentType, controlRoundTrip.ContentType);
        Assert.Equal(control.Authorization, controlRoundTrip.Authorization);
        Assert.Equal(control.BufferWindow, controlRoundTrip.BufferWindow);
        Assert.Equal(control.GetBodyAsUtf8(), controlRoundTrip.GetBodyAsUtf8());

        var media = new CulpeoFrame(
            CulpeoFrameKind.Media,
            new byte[] { 1, 2, 3, 4 },
            contentType: "audio/opus",
            streamId: "s1",
            offset: 3,
            timestamp: 55);

        var mediaRoundTrip = await parser.ParseAsync(await serializer.SerializeAsync(media), CulpeoFrameKind.Media);
        Assert.Equal(media.StreamId, mediaRoundTrip.StreamId);
        Assert.Equal(media.ContentType, mediaRoundTrip.ContentType);
        Assert.Equal(media.Offset, mediaRoundTrip.Offset);
        Assert.Equal(media.Timestamp, mediaRoundTrip.Timestamp);
        Assert.Equal(media.Body.ToArray(), mediaRoundTrip.Body.ToArray());
    }

    [Fact]
    public async Task Session_RejectsNonInitFirstFrame()
    {
        var server = new CulpeoSessionServer();
        var connection = await server.CreateConnectionAsync();

        var result = await connection.ReceiveAsync(new CulpeoFrame(
            CulpeoFrameKind.Control,
            Encoding.UTF8.GetBytes("{\"ts\":1}"),
            @event: "culpeo.ping",
            contentType: "application/json"));

        Assert.True(result.ShouldClose);
        Assert.Equal("protocol-error", result.CloseCode);
        Assert.Equal(CulpeoSessionState.Closed, connection.State);
    }

    [Fact]
    public async Task Session_ReturnsUnsupportedVersion_WithSupportedVersions()
    {
        var server = new CulpeoSessionServer(new CulpeoSessionOptions { SupportedVersions = ["0.3", "0.4"] });
        var connection = await server.CreateConnectionAsync();

        var result = await connection.ReceiveAsync(CreateInitFrame("0.2", requestedBufferWindow: 2000));

        Assert.True(result.ShouldClose);
        var errorFrame = Assert.Single(result.OutboundFrames);
        Assert.Equal("culpeo.init-error", errorFrame.Event);
        Assert.Equal("unsupported-version", errorFrame.Code);
        using var document = JsonDocument.Parse(errorFrame.Body);
        var versions = document.RootElement.GetProperty("supported_versions").EnumerateArray().Select(static item => item.GetString()).ToArray();
        Assert.True(versions.SequenceEqual(["0.3", "0.4"], StringComparer.Ordinal));
    }

    [Fact]
    public async Task Session_EstablishesAndAssignsIds()
    {
        var server = new CulpeoSessionServer(new CulpeoSessionOptions { MaxBufferWindowMs = 2500 });
        var connection = await server.CreateConnectionAsync();

        var result = await connection.ReceiveAsync(CreateInitFrame("0.3", requestedBufferWindow: 5000));

        Assert.False(result.ShouldClose);
        Assert.Equal(CulpeoSessionState.Established, connection.State);
        var ack = Assert.Single(result.OutboundFrames);
        Assert.Equal("culpeo.init-ack", ack.Event);
        Assert.False(string.IsNullOrWhiteSpace(ack.SessionId));
        Assert.Equal(2500, ack.BufferWindow);
        using var document = JsonDocument.Parse(ack.Body);
        var streams = document.RootElement.GetProperty("streams").EnumerateArray().ToArray();
        Assert.Equal(3, streams.Length);
        Assert.All(streams, stream => Assert.False(string.IsNullOrWhiteSpace(stream.GetProperty("id").GetString())));
    }

    [Fact]
    public async Task Session_RejectsInvalidStreamDeclarations()
    {
        var server = new CulpeoSessionServer();
        var connection = await server.CreateConnectionAsync();
        var init = new CulpeoFrame(
            CulpeoFrameKind.Control,
            Encoding.UTF8.GetBytes("""
            {"version":"0.3","streams":[
              {"content_type":"audio/pcm;rate=16000;channels=1;bits=16","type":"input"},
              {"content_type":"audio/pcm;rate=16000;channels=1;bits=16","type":"input"}
            ]}
            """),
            @event: "culpeo.init",
            contentType: "application/json",
            authorization: "Bearer token",
            bufferWindow: 1000);

        var result = await connection.ReceiveAsync(init);

        Assert.True(result.ShouldClose);
        Assert.Equal("invalid-streams", result.CloseCode);
    }

    [Fact]
    public async Task Session_RejectsInitWhenStreamCountExceedsConfiguredMaximum()
    {
        var server = new CulpeoSessionServer(new CulpeoSessionOptions { MaxStreamCount = 2 });
        var connection = await server.CreateConnectionAsync();

        var result = await connection.ReceiveAsync(CreateInitFrame("0.3"));

        Assert.True(result.ShouldClose);
        Assert.Equal("invalid-streams", result.CloseCode);
        var errorFrame = Assert.Single(result.OutboundFrames);
        Assert.Equal("culpeo.init-error", errorFrame.Event);
        Assert.Equal("invalid-streams", errorFrame.Code);
    }

    [Fact]
    public async Task Session_PreservesZeroBufferWindowInInitAck()
    {
        var server = new CulpeoSessionServer(new CulpeoSessionOptions { MaxBufferWindowMs = 2500 });
        var connection = await server.CreateConnectionAsync();

        var result = await connection.ReceiveAsync(CreateInitFrame("0.3", requestedBufferWindow: 0));

        Assert.False(result.ShouldClose);
        var ack = Assert.Single(result.OutboundFrames);
        Assert.Equal(0, ack.BufferWindow);
    }

    [Fact]
    public async Task Session_RejectsPcmStreamMissingRequiredRateParameter()
    {
        var server = new CulpeoSessionServer();
        var connection = await server.CreateConnectionAsync();
        var init = new CulpeoFrame(
            CulpeoFrameKind.Control,
            Encoding.UTF8.GetBytes("""
            {"version":"0.3","streams":[
              {"content_type":"audio/pcm;channels=1;bits=16","type":"input","purpose":"user-voice"},
              {"content_type":"audio/opus","type":"output","purpose":"assistant-voice"},
              {"content_type":"application/json","type":"duplex","purpose":"events"}
            ]}
            """),
            @event: "culpeo.init",
            contentType: "application/json",
            authorization: "Bearer token",
            bufferWindow: 1000);

        var result = await connection.ReceiveAsync(init);

        Assert.True(result.ShouldClose);
        Assert.Equal("protocol-error", result.CloseCode);
        Assert.Equal("culpeo.init-error", Assert.Single(result.OutboundFrames).Event);
    }

    [Fact]
    public async Task Session_RejectsPcmStreamWithInvalidBitsParameter()
    {
        var server = new CulpeoSessionServer();
        var connection = await server.CreateConnectionAsync();
        var init = new CulpeoFrame(
            CulpeoFrameKind.Control,
            Encoding.UTF8.GetBytes("""
            {"version":"0.3","streams":[
              {"content_type":"audio/pcm;rate=16000;channels=1;bits=12","type":"input","purpose":"user-voice"},
              {"content_type":"audio/opus","type":"output","purpose":"assistant-voice"},
              {"content_type":"application/json","type":"duplex","purpose":"events"}
            ]}
            """),
            @event: "culpeo.init",
            contentType: "application/json",
            authorization: "Bearer token",
            bufferWindow: 1000);

        var result = await connection.ReceiveAsync(init);

        Assert.True(result.ShouldClose);
        Assert.Equal("protocol-error", result.CloseCode);
        Assert.Equal("culpeo.init-error", Assert.Single(result.OutboundFrames).Event);
    }

    [Fact]
    public async Task Session_EnforcesDirectionality_ForClientMedia()
    {
        var server = new CulpeoSessionServer();
        var connection = await EstablishSessionAsync(server);
        var outputStreamId = GetStreamId(connection, CulpeoStreamType.Output, "assistant-voice");

        var result = await connection.ReceiveAsync(new CulpeoFrame(
            CulpeoFrameKind.Media,
            new byte[] { 1, 2, 3 },
            contentType: "audio/opus",
            streamId: outputStreamId,
            offset: 0,
            timestamp: 0));

        Assert.True(result.ShouldClose);
        Assert.Equal("protocol-error", result.CloseCode);
    }

    [Fact]
    public async Task OffsetTracking_UsesSampleCountForPcm_AndOneForEncoded()
    {
        var time = new ManualTimeProvider(DateTimeOffset.Parse("2026-05-26T00:00:00Z"));
        var server = new CulpeoSessionServer(new CulpeoSessionOptions { TimeProvider = time });
        var connection = await EstablishSessionAsync(server);
        var inputStreamId = GetStreamId(connection, CulpeoStreamType.Input, "user-voice");
        var outputStreamId = GetStreamId(connection, CulpeoStreamType.Output, "assistant-voice");

        var pcmFrame1 = new CulpeoFrame(CulpeoFrameKind.Media, new byte[320], contentType: "audio/pcm;rate=16000;channels=1;bits=16", streamId: inputStreamId, offset: 0, timestamp: 0);
        var pcmFrame2 = new CulpeoFrame(CulpeoFrameKind.Media, new byte[320], contentType: "audio/pcm;rate=16000;channels=1;bits=16", streamId: inputStreamId, offset: 160, timestamp: 10_000);

        Assert.False((await connection.ReceiveAsync(pcmFrame1)).ShouldClose);
        Assert.False((await connection.ReceiveAsync(pcmFrame2)).ShouldClose);
        Assert.Equal(320, connection.Streams.Single(stream => stream.Id == inputStreamId).CurrentOffset);

        var serverFrame1 = await connection.SendMediaAsync(outputStreamId, new byte[] { 1, 2, 3 }, 5_000);
        var serverFrame2 = await connection.SendMediaAsync(outputStreamId, new byte[] { 4, 5, 6 }, 6_000);
        Assert.Equal(0, serverFrame1.Offset);
        Assert.Equal(1, serverFrame2.Offset);
    }

    [Fact]
    public async Task MediaContentTypeComparison_IsCaseInsensitiveForNames_AndCaseSensitiveForValues()
    {
        var server = new CulpeoSessionServer();
        var connection = await server.CreateConnectionAsync();
        var init = new CulpeoFrame(
            CulpeoFrameKind.Control,
            Encoding.UTF8.GetBytes("""
            {"version":"0.3","streams":[
              {"content_type":"application/x-test;Mode=Fast","type":"input","purpose":"user-voice"},
              {"content_type":"audio/opus","type":"output","purpose":"assistant-voice"},
              {"content_type":"application/json","type":"duplex","purpose":"events"}
            ]}
            """),
            @event: "culpeo.init",
            contentType: "application/json",
            authorization: "Bearer token",
            bufferWindow: 1000);

        var initResult = await connection.ReceiveAsync(init);
        Assert.False(initResult.ShouldClose);
        var inputStreamId = GetStreamId(connection, CulpeoStreamType.Input, "user-voice");

        var accepted = await connection.ReceiveAsync(new CulpeoFrame(
            CulpeoFrameKind.Media,
            new byte[] { 1 },
            contentType: "APPLICATION/X-TEST;mode=Fast",
            streamId: inputStreamId,
            offset: 0,
            timestamp: 0));
        Assert.False(accepted.ShouldClose);

        var rejected = await connection.ReceiveAsync(new CulpeoFrame(
            CulpeoFrameKind.Media,
            new byte[] { 2 },
            contentType: "application/x-test;mode=fast",
            streamId: inputStreamId,
            offset: 1,
            timestamp: 1));
        Assert.True(rejected.ShouldClose);
        Assert.Equal("protocol-error", rejected.CloseCode);
    }

    [Fact]
    public async Task Session_ResumesWithinBufferWindow_AndClampsResumeOffsetToAvailableWindow()
    {
        var time = new ManualTimeProvider(DateTimeOffset.Parse("2026-05-26T00:00:00Z"));
        var server = new CulpeoSessionServer(new CulpeoSessionOptions { TimeProvider = time, MaxBufferWindowMs = 10_000 });
        var original = await EstablishSessionAsync(server, requestedBufferWindow: 10_000);
        var sessionId = original.SessionId!;
        var inputStreamId = GetStreamId(original, CulpeoStreamType.Input, "user-voice");

        await original.ReceiveAsync(new CulpeoFrame(CulpeoFrameKind.Media, new byte[320], contentType: "audio/pcm;rate=16000;channels=1;bits=16", streamId: inputStreamId, offset: 0, timestamp: 0));
        time.Advance(TimeSpan.FromSeconds(3));
        await original.ReceiveAsync(new CulpeoFrame(CulpeoFrameKind.Media, new byte[320], contentType: "audio/pcm;rate=16000;channels=1;bits=16", streamId: inputStreamId, offset: 160, timestamp: 10_000));
        await original.DisconnectAsync();

        time.Advance(TimeSpan.FromSeconds(8));
        var resumed = await server.CreateConnectionAsync();
        var result = await resumed.ReceiveAsync(CreateResumeInitFrame(original, sessionId, 10_000, requestedInputResumeOffset: 0));

        Assert.False(result.ShouldClose);
        var ack = Assert.Single(result.OutboundFrames);
        using var document = JsonDocument.Parse(ack.Body);
        var resumedStream = document.RootElement.GetProperty("streams").EnumerateArray().Single(stream => stream.GetProperty("id").GetString() == inputStreamId);
        Assert.Equal(160, resumedStream.GetProperty("resume_offset").GetInt64());
    }

    [Fact]
    public async Task Session_RejectsExpiredResumption()
    {
        var time = new ManualTimeProvider(DateTimeOffset.Parse("2026-05-26T00:00:00Z"));
        var server = new CulpeoSessionServer(new CulpeoSessionOptions { TimeProvider = time, MaxBufferWindowMs = 1000 });
        var original = await EstablishSessionAsync(server, requestedBufferWindow: 1000);
        var sessionId = original.SessionId!;
        await original.DisconnectAsync();
        time.Advance(TimeSpan.FromSeconds(2));

        var resumed = await server.CreateConnectionAsync();
        var result = await resumed.ReceiveAsync(CreateResumeInitFrame(original, sessionId, 1000, requestedInputResumeOffset: 0));

        Assert.True(result.ShouldClose);
        Assert.Equal("invalid-session", result.CloseCode);
    }

    [Fact]
    public async Task Ping_ProducesPong_WithEchoedTimestampAndServerTimestamp()
    {
        var time = new ManualTimeProvider(DateTimeOffset.Parse("2026-05-26T12:00:00Z"));
        var server = new CulpeoSessionServer(new CulpeoSessionOptions { TimeProvider = time });
        var connection = await EstablishSessionAsync(server);

        var result = await connection.ReceiveAsync(CreatePingFrame(123));

        var pong = Assert.Single(result.OutboundFrames);
        Assert.Equal("culpeo.pong", pong.Event);
        using var document = JsonDocument.Parse(pong.Body);
        Assert.Equal(123, document.RootElement.GetProperty("ts").GetInt64());
        Assert.Equal(time.GetUtcNow().ToUnixTimeMilliseconds() * 1000, document.RootElement.GetProperty("server_ts").GetInt64());
    }

    [Fact]
    public async Task Ping_RateLimitExceeded_IsSilentlyDropped()
    {
        var time = new ManualTimeProvider(DateTimeOffset.Parse("2026-05-26T12:00:00Z"));
        var server = new CulpeoSessionServer(new CulpeoSessionOptions { TimeProvider = time });
        var connection = await EstablishSessionAsync(server);

        for (var i = 0; i < 5; i++)
        {
            var result = await connection.ReceiveAsync(CreatePingFrame(i));
            Assert.False(result.ShouldClose);
            Assert.Equal("culpeo.pong", Assert.Single(result.OutboundFrames).Event);
        }

        var limited = await connection.ReceiveAsync(CreatePingFrame(5));

        Assert.False(limited.ShouldClose);
        Assert.Empty(limited.OutboundFrames);
        Assert.Null(limited.CloseCode);
        Assert.Equal(CulpeoSessionState.Established, connection.State);
    }

    [Fact]
    public async Task Ping_WithinRateLimit_SucceedsAcrossSlidingWindow()
    {
        var time = new ManualTimeProvider(DateTimeOffset.Parse("2026-05-26T12:00:00Z"));
        var server = new CulpeoSessionServer(new CulpeoSessionOptions { TimeProvider = time });
        var connection = await EstablishSessionAsync(server);

        for (var i = 0; i < 5; i++)
        {
            var result = await connection.ReceiveAsync(CreatePingFrame(i));
            Assert.False(result.ShouldClose);
            time.Advance(TimeSpan.FromMilliseconds(200));
        }

        time.Advance(TimeSpan.FromMilliseconds(50));
        var allowed = await connection.ReceiveAsync(CreatePingFrame(99));

        Assert.False(allowed.ShouldClose);
        var pong = Assert.Single(allowed.OutboundFrames);
        Assert.Equal("culpeo.pong", pong.Event);
        Assert.Equal(CulpeoSessionState.Established, connection.State);
    }

    [Fact]
    public async Task AuthRefresh_UsesSingleUseNonce()
    {
        var server = new CulpeoSessionServer();
        var connection = await EstablishSessionAsync(server);

        var refresh = await connection.IssueAuthRefreshAsync();
        using var refreshDocument = JsonDocument.Parse(refresh.Body);
        var nonce = refreshDocument.RootElement.GetProperty("nonce").GetString();

        var success = await connection.ReceiveAsync(new CulpeoFrame(
            CulpeoFrameKind.Control,
            Encoding.UTF8.GetBytes($"{{\"nonce\":\"{nonce}\"}}"),
            @event: "culpeo.auth-response",
            contentType: "application/json",
            authorization: "Bearer rotated"));
        Assert.False(success.ShouldClose);

        var replay = await connection.ReceiveAsync(new CulpeoFrame(
            CulpeoFrameKind.Control,
            Encoding.UTF8.GetBytes($"{{\"nonce\":\"{nonce}\"}}"),
            @event: "culpeo.auth-response",
            contentType: "application/json",
            authorization: "Bearer rotated"));
        Assert.True(replay.ShouldClose);
        Assert.Equal("unauthorized", replay.CloseCode);
    }

    [Fact]
    public async Task AuthRefresh_TimeoutClosesSession()
    {
        var time = new ManualTimeProvider(DateTimeOffset.Parse("2026-05-26T00:00:00Z"));
        var server = new CulpeoSessionServer(new CulpeoSessionOptions { TimeProvider = time, AuthChallengeTimeout = TimeSpan.FromSeconds(5) });
        var connection = await EstablishSessionAsync(server);

        await connection.IssueAuthRefreshAsync();
        time.Advance(TimeSpan.FromSeconds(6));
        var result = await connection.CheckTimeoutsAsync();

        Assert.True(result.ShouldClose);
        Assert.Equal("auth-expired", result.CloseCode);
        Assert.Equal(CulpeoSessionState.Closed, connection.State);
    }

    private static async Task<CulpeoConnection> EstablishSessionAsync(CulpeoSessionServer server, int requestedBufferWindow = 1000)
    {
        var connection = await server.CreateConnectionAsync();
        var result = await connection.ReceiveAsync(CreateInitFrame("0.3", requestedBufferWindow: requestedBufferWindow));
        Assert.False(result.ShouldClose);
        return connection;
    }

    private static CulpeoFrame CreateInitFrame(string version, int requestedBufferWindow = 1000)
    {
        var body = $$"""
        {
          "version":"{{version}}",
          "streams":[
            {"content_type":"audio/pcm;rate=16000;channels=1;bits=16","type":"input","purpose":"user-voice"},
            {"content_type":"audio/opus","type":"output","purpose":"assistant-voice"},
            {"content_type":"application/json","type":"duplex","purpose":"events"}
          ]
        }
        """;

        return new CulpeoFrame(
            CulpeoFrameKind.Control,
            Encoding.UTF8.GetBytes(body),
            @event: "culpeo.init",
            contentType: "application/json",
            authorization: "Bearer token",
            bufferWindow: requestedBufferWindow);
    }

    private static CulpeoFrame CreateResumeInitFrame(CulpeoConnection original, string sessionId, int requestedBufferWindow, long requestedInputResumeOffset)
    {
        var inputId = GetStreamId(original, CulpeoStreamType.Input, "user-voice");
        var outputId = GetStreamId(original, CulpeoStreamType.Output, "assistant-voice");
        var duplexId = GetStreamId(original, CulpeoStreamType.Duplex, "events");

        var body = $$"""
        {
          "version":"0.3",
          "streams":[
            {"id":"{{inputId}}","content_type":"audio/pcm;rate=16000;channels=1;bits=16","type":"input","purpose":"user-voice","resume_offset":{{requestedInputResumeOffset}}},
            {"id":"{{outputId}}","content_type":"audio/opus","type":"output","purpose":"assistant-voice","resume_offset":0},
            {"id":"{{duplexId}}","content_type":"application/json","type":"duplex","purpose":"events","resume_offset":0}
          ]
        }
        """;

        return new CulpeoFrame(
            CulpeoFrameKind.Control,
            Encoding.UTF8.GetBytes(body),
            @event: "culpeo.init",
            contentType: "application/json",
            authorization: "Bearer token",
            sessionId: sessionId,
            bufferWindow: requestedBufferWindow);
    }

    private static CulpeoFrame CreatePingFrame(long ts)
        => new(
            CulpeoFrameKind.Control,
            Encoding.UTF8.GetBytes($"{{\"ts\":{ts}}}"),
            @event: "culpeo.ping",
            contentType: "application/json");

    private static string GetStreamId(CulpeoConnection connection, CulpeoStreamType type, string purpose)
        => connection.Streams.Single(stream => stream.Type == type && string.Equals(stream.Purpose, purpose, StringComparison.Ordinal)).Id;

    private sealed class ManualTimeProvider(DateTimeOffset initialUtcNow) : TimeProvider
    {
        private DateTimeOffset utcNow = initialUtcNow;

        public override DateTimeOffset GetUtcNow() => utcNow;

        public void Advance(TimeSpan amount)
        {
            utcNow = utcNow.Add(amount);
        }
    }
}
