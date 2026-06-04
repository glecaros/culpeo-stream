# AOT publish test does not assert process exit code — false green on publish failure

**Severity:** Medium  
**Location:** `tests/CulpeoStream.AotTests/AotSmokeTests.cs` lines 233–260

## Description

`PublishAot_Core_ZeroIlcWarnings` runs `dotnet publish` as a child process and then scans its output for ILC trim warning lines. It does not check whether the publish command actually succeeded:

```csharp
using var process = Process.Start(psi);
// ...
process.WaitForExit(300_000);

var combinedOutput = stdout + "\n" + stderr;
var ilcWarnings = combinedOutput
    .Split('\n', ...)
    .Where(line =>
        (line.Contains("warning IL") || line.Contains("Trim analysis warning")) &&
        line.Contains("CulpeoStream") && ...)
    .ToList();

Assert.True(ilcWarnings.Count == 0, ...);  // ← no exit-code check
```

If `dotnet publish` exits with a non-zero code for any reason (missing `TrimmerRoots.xml`, compilation error, insufficient disk space, missing runtime pack, etc.), the output will contain MSBuild-level errors rather than ILC warning lines. The `ilcWarnings` list will be empty, the assertion passes, and the test reports green — incorrectly implying that the AOT publish produced zero warnings when it produced zero *output* because it crashed.

This masks the `TrimmerRoots.xml` bug (filed separately) and any other publish-time failure.

## Impact

CI passes even when AOT publish is completely broken. The "zero ILC warnings" invariant is never actually validated.

## Suggested Fix

Assert that the process exited within the timeout and with exit code 0, before inspecting the ILC warning output:

```csharp
bool exited = process.WaitForExit(300_000);

Assert.True(exited,
    $"dotnet publish timed out after 5 minutes.\nOutput:\n{combinedOutput[..Math.Min(4000, combinedOutput.Length)]}");

Assert.True(process.ExitCode == 0,
    $"dotnet publish failed with exit code {process.ExitCode}.\n" +
    $"Output:\n{combinedOutput[..Math.Min(4000, combinedOutput.Length)]}");

// Only then check for ILC warnings
Assert.True(ilcWarnings.Count == 0, ...);
```
