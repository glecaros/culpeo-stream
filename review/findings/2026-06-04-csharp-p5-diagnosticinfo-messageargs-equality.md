# DiagnosticInfo.MessageArgs excluded from Equals — incremental caching emits stale diagnostics

**Severity:** Medium  
**Location:** `src/CulpeoStream.SourceGen/Models/HandlerDescriptor.cs` lines 88–95

## Description

`DiagnosticInfo.Equals` compares `Id`, `Message`, `Severity`, `FilePath`, `Line`, and `Column` — but not `MessageArgs`:

```csharp
public bool Equals(DiagnosticInfo? other) =>
    other is not null &&
    Id == other.Id &&
    Message == other.Message &&
    Severity == other.Severity &&
    FilePath == other.FilePath &&
    Line == other.Line &&
    Column == other.Column;
    // MessageArgs is NOT compared
```

`HandlerDescriptor.Equals` compares diagnostics via `Diagnostics.SequenceEqual(other.Diagnostics)`, which calls `DiagnosticInfo.Equals`. The incremental generator uses `HandlerDescriptor` equality to decide whether to skip re-running `RegisterSourceOutput`. If `MessageArgs` differs but everything else is the same, the two descriptors are considered equal and the previous cached result (including the previously-reported diagnostic with old args) is reused.

**Concrete scenario:** A handler class named `OldHandler` gets renamed to `NewHandler`. The CULPEO002 diagnostic for it has `MessageArgs = ["OldHandler"]`. After renaming, `MessageArgs = ["NewHandler"]`. Since `Message` also changes (`"Class 'OldHandler'..."` vs `"Class 'NewHandler'..."`), in this specific case the bug is masked. But if the `Message` is templated separately from `MessageArgs` (and the full message string is constructed at reporting time by Roslyn from the format string + args), the `Message` field stored in `DiagnosticInfo` could be the format string `"Class '{0}' is annotated..."` rather than the fully expanded string — making it identical across renames, while only `MessageArgs[0]` changes. In that case the incremental cache would serve the stale "OldHandler" diagnostic text.

Additionally, `MessageArgs` being a mutable `object[]` array means two `DiagnosticInfo` instances with different array references but equal contents are treated as unequal by `object.ReferenceEquals` (which is what array comparison uses by default) — and also cannot be properly hashed — making the overall equality contract inconsistent.

## Impact

Incremental rebuilds may report stale diagnostic messages (wrong class name, wrong stream ID) without re-running analysis. This is a correctness bug in IDE scenarios: after renaming a class, the error tooltip in the IDE could still show the old class name until a full rebuild.

## Suggested Fix

Include `MessageArgs` in both `Equals` and `GetHashCode`, comparing by content (not reference):

```csharp
public bool Equals(DiagnosticInfo? other) =>
    other is not null &&
    Id == other.Id &&
    Message == other.Message &&
    Severity == other.Severity &&
    FilePath == other.FilePath &&
    Line == other.Line &&
    Column == other.Column &&
    (MessageArgs == other.MessageArgs ||
     (MessageArgs is not null && other.MessageArgs is not null &&
      MessageArgs.SequenceEqual(other.MessageArgs)));
```

Or change `MessageArgs` to `ImmutableArray<object>` to get structural equality for free.
