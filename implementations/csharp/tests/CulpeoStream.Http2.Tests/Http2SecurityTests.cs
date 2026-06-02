using System.Buffers.Binary;
using CulpeoStream.Core;
using CulpeoStream.Http2;

namespace CulpeoStream.Http2.Tests;

/// <summary>
/// Security regression tests for SEC-024 (signed-int cast bypass) and
/// SEC-031 (AllowHttp2Cleartext warning).
/// </summary>
public sealed class Http2SecurityTests
{
    // ── SEC-024: uint bounds-check tests ────────────────────────────────────

    /// <summary>
    /// A payload length exactly equal to the limit must succeed and return
    /// the correct bytes. Verifies the boundary condition (<=) is correct.
    /// </summary>
    [Fact]
    public async Task ReadFrame_PayloadLength_ExactlyAtLimit_Succeeds()
    {
        const int limit = 64; // small limit to keep the test fast
        var payload = new byte[limit];
        for (var i = 0; i < limit; i++) payload[i] = (byte)(i & 0xFF);

        var ms = new MemoryStream();
        // Write envelope manually: [type:1][length:4-BE][payload:limit]
        ms.WriteByte(0x01);
        var lengthBuf = new byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(lengthBuf, (uint)limit);
        ms.Write(lengthBuf);
        ms.Write(payload);
        ms.Position = 0;

        var (typeOctet, result) = await Http2FrameReader.ReadFrameAsync(ms, limit);

        Assert.Equal(0x01, typeOctet);
        Assert.Equal(payload, result);
    }

    /// <summary>
    /// A payload length one byte over the limit must throw
    /// <see cref="CulpeoProtocolException"/> with code "frame-too-large".
    /// </summary>
    [Fact]
    public async Task ReadFrame_PayloadLength_OneOverLimit_ThrowsProtocolException()
    {
        const int limit = 64;
        const uint overLimit = limit + 1;

        var ms = new MemoryStream();
        ms.WriteByte(0x01);
        var lengthBuf = new byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(lengthBuf, overLimit);
        ms.Write(lengthBuf);
        // No payload bytes needed — the check fires before any read.
        ms.Position = 0;

        var ex = await Assert.ThrowsAsync<CulpeoProtocolException>(
            () => Http2FrameReader.ReadFrameAsync(ms, limit).AsTask());

        Assert.Equal("frame-too-large", ex.Code);
    }

    /// <summary>
    /// SEC-024 regression: a 4-byte length of 0x80000000 (2 GiB) would cast to
    /// a negative <see cref="int"/> and bypass the old bounds check.
    /// Must throw <see cref="CulpeoProtocolException"/> with code "frame-too-large".
    /// </summary>
    [Fact]
    public async Task ReadFrame_PayloadLength_0x80000000_ThrowsProtocolException()
    {
        const uint maliciousLength = 0x80000000u; // 2 GiB — was negative int pre-fix

        var ms = new MemoryStream();
        ms.WriteByte(0x02);
        var lengthBuf = new byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(lengthBuf, maliciousLength);
        ms.Write(lengthBuf);
        ms.Position = 0;

        var ex = await Assert.ThrowsAsync<CulpeoProtocolException>(
            () => Http2FrameReader.ReadFrameAsync(ms, Http2FrameReader.DefaultMaxPayloadBytes).AsTask());

        Assert.Equal("frame-too-large", ex.Code);
    }

    /// <summary>
    /// SEC-024 regression: the maximum possible uint value (0xFFFFFFFF ≈ 4 GiB)
    /// must be rejected by the bounds check.
    /// </summary>
    [Fact]
    public async Task ReadFrame_PayloadLength_0xFFFFFFFF_ThrowsProtocolException()
    {
        const uint maliciousLength = 0xFFFFFFFFu;

        var ms = new MemoryStream();
        ms.WriteByte(0x02);
        var lengthBuf = new byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(lengthBuf, maliciousLength);
        ms.Write(lengthBuf);
        ms.Position = 0;

        var ex = await Assert.ThrowsAsync<CulpeoProtocolException>(
            () => Http2FrameReader.ReadFrameAsync(ms, Http2FrameReader.DefaultMaxPayloadBytes).AsTask());

        Assert.Equal("frame-too-large", ex.Code);
    }

    // ── SEC-031: AllowHttp2Cleartext warning test ────────────────────────────

    /// <summary>
    /// Constructing <see cref="CulpeoHttp2Client"/> with
    /// <c>AllowHttp2Cleartext = true</c> must write a visible warning to stderr.
    /// </summary>
    [Fact]
    public async Task CulpeoHttp2Client_AllowHttp2Cleartext_WritesStderrWarning()
    {
        // Capture Console.Error output.
        var originalErr = Console.Error;
        var captured = new StringWriter();
        Console.SetError(captured);

        try
        {
#pragma warning disable CS0618 // deliberate test of the obsolete opt-in
            var opts = new CulpeoHttp2ClientOptions { AllowHttp2Cleartext = true };
#pragma warning restore CS0618
            var client = new CulpeoHttp2Client(opts);
            // No need to dispose — we only care about the constructor side-effect.
            // The client holds an HttpClient but no connections are opened.
            await client.DisposeAsync();
        }
        finally
        {
            // Always restore Console.Error — do NOT leave it redirected.
            Console.SetError(originalErr);
        }

        var stderr = captured.ToString();
        Assert.Contains("WARNING", stderr, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("Http2UnencryptedSupport", stderr);
        Assert.Contains("process-wide", stderr, StringComparison.OrdinalIgnoreCase);
    }
}
