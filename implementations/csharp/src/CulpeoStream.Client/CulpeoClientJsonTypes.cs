using System.Text.Json.Serialization;

namespace CulpeoStream.Client;

// ── AOT-safe JSON types (Finding 4) ──────────────────────────────────────────
// These named records replace the anonymous types previously used in
// CulpeoStreamClient, which caused reflection-based JSON serialization that is
// unsafe under NativeAOT / ILC trim analysis.

/// <summary>Body of a <c>culpeo.auth-response</c> frame.</summary>
internal sealed record AuthResponseBody(string Nonce)
{
    [JsonPropertyName("nonce")]
    public string Nonce { get; init; } = Nonce;
}

/// <summary>Body of a <c>culpeo.pong</c> frame.</summary>
internal sealed record PongBody(long Ts, long ServerTs)
{
    [JsonPropertyName("ts")]
    public long Ts { get; init; } = Ts;

    [JsonPropertyName("server_ts")]
    public long ServerTs { get; init; } = ServerTs;
}

/// <summary>
/// <see cref="System.Text.Json.Serialization.JsonSerializerContext"/> covering all types
/// serialized by <see cref="CulpeoStreamClient"/>. Provides source-generated, trim-safe,
/// NativeAOT-compatible JSON serialization.
/// </summary>
[JsonSourceGenerationOptions(
    PropertyNamingPolicy = JsonKnownNamingPolicy.SnakeCaseLower,
    GenerationMode = JsonSourceGenerationMode.Serialization | JsonSourceGenerationMode.Metadata)]
[JsonSerializable(typeof(AuthResponseBody))]
[JsonSerializable(typeof(PongBody))]
internal sealed partial class CulpeoClientJsonContext : JsonSerializerContext { }
