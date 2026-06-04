# TrimmerRoots.xml referenced in AotTests project but file does not exist

**Severity:** High  
**Location:** `tests/CulpeoStream.AotTests/CulpeoStream.AotTests.csproj` line with `<TrimmerRootDescriptor>`

## Description

The project file contains:

```xml
<TrimmerRootDescriptor>$(MSBuildThisFileDirectory)TrimmerRoots.xml</TrimmerRootDescriptor>
```

The referenced file `tests/CulpeoStream.AotTests/TrimmerRoots.xml` does not exist in the repository.

When `dotnet publish -c Release -r linux-x64 --self-contained -p:PublishAot=true` is run, MSBuild attempts to pass `TrimmerRoots.xml` to the ILC linker. A missing trimmer root descriptor causes the publish step to fail with:

```
error : The TrimmerRootDescriptor file 'TrimmerRoots.xml' does not exist.
```

This means the Phase 5 "Definition of Done" requirement — *"dotnet publish … produces a binary with zero ILC trim warnings"* — can never be validated because the publish itself always fails. The `PublishAot_Core_ZeroIlcWarnings` test, which runs `dotnet publish` and filters the output for ILC warnings, would pass vacuously because the failure output contains no matching ILC warning lines (the error is MSBuild-level, not an ILC `warning IL` line).

## Impact

AOT publish is broken end-to-end. The Phase 5 invariant that the library is NativeAOT-safe cannot be demonstrated or tested. Any CI pipeline that runs `dotnet publish` on this project will fail.

## Suggested Fix

Either create the missing `TrimmerRoots.xml` file (typically it roots the test entry point and xUnit infrastructure that the linker cannot discover statically):

```xml
<linker>
  <assembly fullname="CulpeoStream.AotTests" preserve="all" />
  <assembly fullname="xunit.core" preserve="all" />
</linker>
```

Or remove the `<TrimmerRootDescriptor>` line if it was added speculatively — the ILC compiler can usually discover roots through `PublishAot` entry-point analysis without a manual descriptor.
