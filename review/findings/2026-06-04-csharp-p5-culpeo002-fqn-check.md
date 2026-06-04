# CULPEO002 check uses simple interface name, not fully-qualified — false negatives

**Severity:** Medium  
**Location:** `src/CulpeoStream.SourceGen/Generators/HandlerGenerator.cs` lines 147–151; also lines 175, 184

## Description

The CULPEO002 check that validates whether the class implements `ICulpeoStreamHandler` compares only the simple `Name` of each interface symbol:

```csharp
foreach (var iface in symbol.AllInterfaces)
{
    if (iface.Name == "ICulpeoStreamHandler") { implementsHandler = true; break; }
}
```

The same pattern is used to detect `[DeclareStream]` attributes (line 175, `attr.AttributeClass?.Name == DeclareStreamAttrName`) and to validate the field type (line 184, `field.Type.Name != "StreamDeclaration"`).

Simple-name comparison has two failure modes:

1. **False negative (CULPEO002 suppressed):** A class that implements a user-defined `ICulpeoStreamHandler` from a different namespace (e.g., `MyApp.Mocks.ICulpeoStreamHandler`) will pass the check without implementing `CulpeoStream.AspNetCore.ICulpeoStreamHandler`. The generator emits source that calls the generated methods as if they satisfy the real interface, but the generated partial class will not actually satisfy `CulpeoStream.AspNetCore.ICulpeoStreamHandler`, producing confusing downstream CS0535 errors.

2. **False positive / incorrect DeclareStream handling:** If the user has a type named `StreamDeclaration` in a different namespace, or an attribute named `DeclareStreamAttribute` in their own namespace, the generator will either silently skip or incorrectly process those fields.

## Impact

Users with naming collisions get wrong diagnostics or incorrectly generated code. More critically, a class that does not implement the protocol's handler interface can silently receive generated code that will fail at the use site.

## Suggested Fix

Use fully-qualified name comparison for all three checks:

```csharp
// Interface check
if (iface.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat)
    == "global::CulpeoStream.AspNetCore.ICulpeoStreamHandler")

// Attribute check — compare to the FQN emitted at post-init
if (attr.AttributeClass?.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat)
    == "global::CulpeoStream.Generated.DeclareStreamAttribute")

// Field type check
if (field.Type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat)
    != "global::CulpeoStream.Core.StreamDeclaration")
```

Alternatively, capture the `INamedTypeSymbol` for `ICulpeoStreamHandler` from the compilation in the `transform` step using `context.SemanticModel.Compilation.GetTypeByMetadataName("CulpeoStream.AspNetCore.ICulpeoStreamHandler")` and compare by symbol identity.
