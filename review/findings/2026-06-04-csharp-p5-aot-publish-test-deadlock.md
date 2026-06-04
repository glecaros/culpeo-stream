# AOT publish test can deadlock reading stdout before stderr

**Severity:** High  
**Location:** `tests/CulpeoStream.AotTests/AotSmokeTests.cs` lines 236–238

## Description

The `PublishAot_Core_ZeroIlcWarnings` test spawns a child `dotnet publish` process with both stdout and stderr redirected, then reads them sequentially and synchronously:

```csharp
var stdout = process.StandardOutput.ReadToEnd();   // blocks until stdout EOF
var stderr = process.StandardError.ReadToEnd();    // only starts after stdout is done
process.WaitForExit(300_000);
```

This is the classic redirected-I/O deadlock: `ReadToEnd()` on stdout blocks until the child process closes its stdout pipe. The child process writes to both stdout and stderr. If stderr fills its OS pipe buffer (typically 64 KB on Linux) before stdout is closed, the child will block attempting to write to the stderr pipe. The parent is still blocked waiting on stdout. Neither side makes progress — a permanent deadlock that hangs for the full 300-second timeout.

An AOT publish of a non-trivial .NET project produces many kilobytes of diagnostic output across both streams, making this a realistic deadlock scenario in CI.

## Impact

The test hangs for 5 minutes then `process.WaitForExit` returns `false` (timeout). The test does not check the timeout return value, so execution continues, `stderr` is read (returning empty or partial output because the process is still alive), and the ILC warning filter runs on incomplete data — potentially passing despite real warnings. In the deadlock case the test takes 5 minutes and the results are unreliable.

## Suggested Fix

Read both streams concurrently using `async` tasks or the `OutputDataReceived`/`ErrorDataReceived` event callbacks:

```csharp
var stdoutTask = process.StandardOutput.ReadToEndAsync();
var stderrTask  = process.StandardError.ReadToEndAsync();
process.WaitForExit(300_000);
var stdout = await stdoutTask;
var stderr  = await stderrTask;
```

Or use `BeginOutputReadLine` / `BeginErrorReadLine` with `StringBuilder` collectors before calling `WaitForExit`. Either approach drains both pipes concurrently, eliminating the deadlock.

Additionally, assert that the process exited cleanly:

```csharp
Assert.True(
    process.HasExited,
    $"dotnet publish timed out after 5 minutes. Partial output:\n{...}");
Assert.Equal(0, process.ExitCode);
```
