## Finding: Serializers do not validate header values before serializing

**Severity:** Low
**Target:** C#, TypeScript
**Phase:** Phase 1

### Description

Both the C# `CulpeoFrameSerializer` and the TypeScript `serializeTextFrame`/`serializeBinaryFrame` functions write header values directly into the output without checking for CR, LF, or NUL bytes. The C++ `serialize_frame` correctly validates each header via `valid_header_name` and `valid_header_value` before writing.

**C# `CulpeoFrameSerializer.AppendHeader`:**
```csharp
private static void AppendHeader(StringBuilder builder, string name, string? value)
{
    if (string.IsNullOrEmpty(value)) return;
    builder.Append(name);
    builder.Append(": ");
    builder.Append(value);      // no CR/LF/NUL check
    builder.Append("\r\n");
}
```

**TypeScript `serializeTextFrame`:**
```typescript
data: `${headers.map(([name, value]) => `${name}: ${value}\r\n`).join("")}\r\n...`
// no validation of name or value before interpolation
```

In the current session implementations, all outgoing header values are either hardcoded constants (`"culpeo.init-ack"`, `"application/json"`) or generated from cryptographically-safe hex sources. There is no active exploit path in the existing session code. However:

1. The `CulpeoFrame` constructor (C#) and frame type objects (TypeScript) are part of the **public API**. Library consumers who construct frames manually and serialize them with the provided serializers bypass any validation.
2. In the TypeScript `CulpeoClientSession.sendEvent`, the `event` parameter is a caller-supplied `string` that is written directly into a header:
   ```typescript
   await this.dispatch({ kind: "control", event, headers: { event, ... }, body });
   ```
   If `event` contains `\r\n` (e.g., `"app.event\r\nEvent: culpeo.close\r\nCode: normal"`), the serialized frame injects a synthetic close frame header.

### Attack Scenario

**TypeScript `sendEvent` injection:**
1. Application code routes user-influenced data through `sendEvent`:
   ```typescript
   await session.sendEvent(userInputEventName, body);
   ```
2. An attacker supplies `userInputEventName = "x\r\nEvent: culpeo.close\r\nCode: auth-expired\r\n"`.
3. The serialized text frame output contains injected headers that a conformant parser on the receiving end interprets as a `culpeo.close` frame.
4. The peer session closes unexpectedly, disrupting the session.

**C# public API misuse:**
1. A library consumer constructs `new CulpeoFrame(..., reason: userInput)`.
2. `userInput` contains `"\r\nCode: auth-expired"`.
3. Serialization produces a frame with an injected `Code` header that overrides the intended protocol semantics.

### Impact

Low in the existing session code where headers are fully controlled. Medium if library consumers pass user-controlled strings to `sendEvent` or the `CulpeoFrame` constructor without sanitizing first.

### Proposed Mitigation

**TypeScript `sendEvent`:** Validate the event name before use:
```typescript
public async sendEvent(event: string, body: ..., streamId?: string): Promise<void> {
    if (forbiddenHeaderCharacters.test(event)) {
        throw new CulpeoError("protocol-error", "Event name contains forbidden characters.");
    }
    ...
}
```

**TypeScript serializer (defense-in-depth):** Validate all header names and values in `frameToHeaders` output before interpolating.

**C# serializer:** Add a validation call in `AppendHeader`:
```csharp
private static void AppendHeader(StringBuilder builder, string name, string? value)
{
    if (string.IsNullOrEmpty(value)) return;
    if (value.ContainsAny(['\r', '\n', '\0']))
        throw new InvalidOperationException($"Header '{name}' value contains forbidden bytes.");
    ...
}
```

The C++ `serialize_frame` is already correct and serves as the reference pattern for the other two implementations.

### Spec Reference

§4.1 — Frame Header Format: "Implementations MUST reject any frame that contains CR, LF, or NUL in a header name or value."
