# SafeMethodSuffix collision generates uncompilable code with no diagnostic

**Severity:** High  
**Location:** `src/CulpeoStream.SourceGen/Models/StreamDescriptor.cs` lines 62–79; `src/CulpeoStream.SourceGen/Generators/CodeGenerator.cs` lines 124–130, 157–164

## Description

`StreamDescriptor.SafeMethodSuffix` converts a stream ID to a C# identifier by capitalising after `-`, `_`, and `.` separators. Two stream IDs that differ only in separator character produce identical suffixes:

| Stream ID | SafeMethodSuffix | Generated method |
|-----------|-----------------|-----------------|
| `"audio-in"` | `AudioIn` | `protected virtual Task OnAudioInAsync(...)` |
| `"audio.in"` | `AudioIn` | `protected virtual Task OnAudioInAsync(...)` ← **duplicate** |

The generator deduplicates on raw stream IDs (via `seenIds.Add(id)`) so both streams pass the CULPEO001 check. The generated partial class then contains two identical `protected virtual Task OnAudioInAsync(...)` declarations, producing compiler error CS0111 ("Type already defines a member called 'OnAudioInAsync' with the same parameter types").

The generated `HandleMediaAsync` switch expression is also affected: both case arms would call the same method, so only one stream routes correctly even if the CS0111 error were suppressed.

Example input that triggers the bug:

```csharp
[CulpeoStreamHandler]
public partial class MyHandler : ICulpeoStreamHandler
{
    [DeclareStream("audio-in", CulpeoStreamType.Input, OffsetType.Byte, "audio/opus")]
    private StreamDeclaration _audioIn = default!;

    [DeclareStream("audio.in", CulpeoStreamType.Duplex, OffsetType.Byte, "audio/opus")]
    private StreamDeclaration _audioDotIn = default!;
    // ...
}
```

The user sees a compiler error in the generated `.g.cs` file with no associated CULPEO diagnostic explaining why.

## Impact

The consuming project fails to compile with a confusing error pointing at the generator's output file. There is no CULPEO diagnostic to guide the user to the real problem. The generator silently emits broken code.

## Suggested Fix

After computing `SafeMethodSuffix` for all streams, check for collisions and emit a new diagnostic (e.g. CULPEO004) on the second stream declaring the colliding suffix. Do not emit source when a suffix collision is present. Example check in `TransformHandler` (after the per-field loop):

```csharp
var seenSuffixes = new HashSet<string>(StringComparer.Ordinal);
foreach (var stream in streams)
{
    if (!seenSuffixes.Add(stream.SafeMethodSuffix))
    {
        diagnostics.Add(new DiagnosticInfo(
            "CULPEO004",
            $"Streams produce identical method name 'On{stream.SafeMethodSuffix}Async'; rename one stream ID.",
            DiagnosticSeverity.Error, ...));
    }
}
```
