# CulpeoStream.Client uses reflection-based JSON serialization — not AOT-safe

**Severity:** High  
**Location:** `src/CulpeoStream.Client/CulpeoStreamClient.cs` lines 284, 535, 563

## Description

Three call sites in `CulpeoStreamClient` use `JsonSerializer.Serialize` with types that are opaque to the NativeAOT trimmer:

**Line 284 — `object?` parameter:**
```csharp
public async Task SendEventAsync(string eventName, object? body, ...)
{
    byte[] bodyBytes = body is null
        ? "{}"u8.ToArray()
        : Encoding.UTF8.GetBytes(JsonSerializer.Serialize(body));  // ← object? — IL2026
```
The trimmer cannot statically determine the concrete type of `body`, so this emits `warning IL2026: Using member 'JsonSerializer.Serialize<TValue>' which has 'RequiresUnreferencedCodeAttribute'`. Under NativeAOT the serialization of an unknown type will fail at runtime with `NotSupportedException`.

**Lines 535 and 563 — anonymous types:**
```csharp
var nonceBodyBytes = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(new { nonce }));

var pongBodyBytes = Encoding.UTF8.GetBytes(
    JsonSerializer.Serialize(new { ts, server_ts = serverTs }));
```
Anonymous types are compiler-generated classes whose metadata is stripped by the NativeAOT trimmer. `JsonSerializer.Serialize` on an anonymous type requires runtime reflection that is unavailable under NativeAOT, producing `warning IL3050: Using member '...' which has 'RequiresDynamicCodeAttribute'` and a runtime crash.

The Phase 5 AOT test project only covers `CulpeoStream.Core`. It does not reference or test `CulpeoStream.Client`, so these violations are not caught by the existing AOT validation.

## Impact

Any application that references `CulpeoStream.Client` and publishes with `PublishAot=true` or `PublishTrimmed=true` will:
- Receive multiple ILC trim warnings (`IL2026`, `IL3050`) during publish, breaking a "zero ILC warnings" policy.
- Encounter `NotSupportedException` or `InvalidOperationException` at runtime when `SendEventAsync` is called with a non-null body, or when a ping or auth-refresh is processed.

## Suggested Fix

Replace anonymous-type and `object?` serialization with explicit manual JSON construction using `Utf8JsonWriter` (already used in `CulpeoSession.cs` for the same purpose) or a `[JsonSerializable]`-annotated source-generated `JsonSerializerContext`:

```csharp
// Replace JsonSerializer.Serialize(new { nonce }) with:
using var buf = new ArrayBufferWriter<byte>();
using var w = new Utf8JsonWriter(buf);
w.WriteStartObject();
w.WriteString("nonce", nonce);
w.WriteEndObject();
w.Flush();
var nonceBodyBytes = buf.WrittenSpan.ToArray();
```

For `SendEventAsync(object? body)`, the signature itself is AOT-hostile. Replace it with `SendEventAsync(string jsonBody = "{}")` (accepting pre-serialised JSON, matching the server-side `ICulpeoStreamSession.SendEventAsync` signature) and document that callers are responsible for serializing their own payloads using a source-generated context.
