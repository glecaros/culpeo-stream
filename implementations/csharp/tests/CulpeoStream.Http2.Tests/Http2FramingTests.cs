using System.Buffers.Binary;
using System.Text;
using CulpeoStream.Core;
using CulpeoStream.Http2;

namespace CulpeoStream.Http2.Tests;

/// <summary>
/// Unit tests for <see cref="Http2FrameWriter"/> and <see cref="Http2FrameReader"/>.
/// These tests exercise the wire-level framing layer in isolation (no network I/O).
/// </summary>
public sealed class Http2FramingTests
{
    // ── Test 1: control frame round-trip ─────────────────────────────────────

    [Fact]
    public async Task WriteFrame_ThenReadFrame_RoundTrips_ControlFrame()
    {
        const byte controlType = 0x01;
        var payload = Encoding.UTF8.GetBytes("Event: culpeo.ping\r\n\r\n{}");

        var ms = new MemoryStream();
        await Http2FrameWriter.WriteFrameAsync(ms, controlType, payload, default);

        ms.Position = 0;
        var (typeOctet, roundTripped) = await Http2FrameReader.ReadFrameAsync(ms);

        Assert.Equal(controlType, typeOctet);
        Assert.Equal(payload, roundTripped);
    }

    // ── Test 2: media frame round-trip ────────────────────────────────────────

    [Fact]
    public async Task WriteFrame_ThenReadFrame_RoundTrips_MediaFrame()
    {
        const byte mediaType = 0x02;
        // Simulate a binary audio payload
        var payload = new byte[256];
        for (var i = 0; i < payload.Length; i++) payload[i] = (byte)(i % 256);

        var ms = new MemoryStream();
        await Http2FrameWriter.WriteFrameAsync(ms, mediaType, payload, default);

        ms.Position = 0;
        var (typeOctet, roundTripped) = await Http2FrameReader.ReadFrameAsync(ms);

        Assert.Equal(mediaType, typeOctet);
        Assert.Equal(payload, roundTripped);
    }

    // ── Test 3: frame-too-large throws CulpeoProtocolException ───────────────

    [Fact]
    public async Task ReadFrame_PayloadExceedsMax_ThrowsProtocolException()
    {
        const int maxPayload = 100;
        const int actualPayload = 101; // one byte over the limit

        var ms = new MemoryStream();
        // Write a frame with a payload larger than maxPayload
        var oversizedPayload = new byte[actualPayload];
        await Http2FrameWriter.WriteFrameAsync(ms, 0x01, oversizedPayload, default);

        ms.Position = 0;
        var ex = await Assert.ThrowsAsync<CulpeoProtocolException>(
            () => Http2FrameReader.ReadFrameAsync(ms, maxPayload).AsTask());

        Assert.Equal("frame-too-large", ex.Code);
    }

    // ── Test 4: empty payload round-trips correctly ───────────────────────────

    [Fact]
    public async Task WriteFrame_EmptyPayload_RoundTrips()
    {
        const byte controlType = 0x01;
        var payload = Array.Empty<byte>();

        var ms = new MemoryStream();
        await Http2FrameWriter.WriteFrameAsync(ms, controlType, payload, default);

        // Verify wire bytes: 1 byte type + 4 bytes length (zero) = 5 bytes total
        Assert.Equal(5, ms.Length);

        ms.Position = 0;
        var (typeOctet, roundTripped) = await Http2FrameReader.ReadFrameAsync(ms);

        Assert.Equal(controlType, typeOctet);
        Assert.Empty(roundTripped);
    }

    // ── Test 5: unexpected EOF throws EndOfStreamException ───────────────────

    [Fact]
    public async Task ReadFrame_UnexpectedEof_ThrowsEndOfStreamException()
    {
        // Write an incomplete frame: only the 5-byte header, no payload
        const byte mediaType = 0x02;
        var ms = new MemoryStream();

        // Manually write header: type=0x02, length=10 (but provide no payload)
        ms.WriteByte(mediaType);
        var lengthBytes = new byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(lengthBytes, 10); // claims 10 bytes payload
        ms.Write(lengthBytes);
        // Do NOT write the payload — stream ends here

        ms.Position = 0;
        await Assert.ThrowsAsync<EndOfStreamException>(
            () => Http2FrameReader.ReadFrameAsync(ms).AsTask());
    }

    // ── Additional: verify on-wire byte layout ────────────────────────────────

    [Fact]
    public async Task WriteFrame_VerifiesWireLayout()
    {
        // [type:0x01][length-BE:3 bytes = 00 00 00 03][payload: AA BB CC]
        var payload = new byte[] { 0xAA, 0xBB, 0xCC };
        var ms = new MemoryStream();
        await Http2FrameWriter.WriteFrameAsync(ms, 0x01, payload, default);

        var bytes = ms.ToArray();
        Assert.Equal(8, bytes.Length); // 1 + 4 + 3
        Assert.Equal(0x01, bytes[0]);
        Assert.Equal(0x00, bytes[1]);
        Assert.Equal(0x00, bytes[2]);
        Assert.Equal(0x00, bytes[3]);
        Assert.Equal(0x03, bytes[4]);
        Assert.Equal(0xAA, bytes[5]);
        Assert.Equal(0xBB, bytes[6]);
        Assert.Equal(0xCC, bytes[7]);
    }

    // ── Additional: multiple sequential frames in one stream ──────────────────

    [Fact]
    public async Task WriteMultipleFrames_ReadBack_InOrder()
    {
        var ms = new MemoryStream();

        var frame1 = Encoding.UTF8.GetBytes("frame-one");
        var frame2 = Encoding.UTF8.GetBytes("frame-two");
        var frame3 = new byte[] { 0x01, 0x02, 0x03 };

        await Http2FrameWriter.WriteFrameAsync(ms, 0x01, frame1, default);
        await Http2FrameWriter.WriteFrameAsync(ms, 0x02, frame2, default);
        await Http2FrameWriter.WriteFrameAsync(ms, 0x01, frame3, default);

        ms.Position = 0;

        var (t1, p1) = await Http2FrameReader.ReadFrameAsync(ms);
        var (t2, p2) = await Http2FrameReader.ReadFrameAsync(ms);
        var (t3, p3) = await Http2FrameReader.ReadFrameAsync(ms);

        Assert.Equal(0x01, t1); Assert.Equal(frame1, p1);
        Assert.Equal(0x02, t2); Assert.Equal(frame2, p2);
        Assert.Equal(0x01, t3); Assert.Equal(frame3, p3);
    }
}
