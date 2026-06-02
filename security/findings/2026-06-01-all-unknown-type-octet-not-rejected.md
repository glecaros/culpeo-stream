# [SEC-032] All implementations: unknown type octet (not 0x01 or 0x02) passed to application layer without rejection

**Severity:** Low  
**Component:** C++ | C# | TypeScript  
**Phase:** 4  
**Status:** Open  

## Description

Addendum C.3 of the spec defines exactly two valid type-octet values:

| Value  | Meaning                          |
|--------|----------------------------------|
| `0x01` | Control / event frame            |
| `0x02` | Media frame (headers + raw bytes)|

All three implementations decode the type octet from the frame and pass it
through to the application layer without checking whether it is one of these
two valid values.

**C++** (`RecvState::drain_frames`):
```cpp
uint8_t type_byte = buf[4];
// … no validation …
result.emplace_back(type_byte, std::move(payload));
```

**C#** (`Http2FrameReader.ReadFrameAsync`):
```csharp
var typeOctet = header[0];
// … no validation …
return (typeOctet, payload);
```

**TypeScript** (`framing.ts`, `decodeFrame`):
```typescript
const typeOctet = buf.readUInt8(0);
// … no validation …
return { typeOctet, payload, bytesConsumed };
```

Application-layer handlers receive frames with arbitrary type bytes and must
implement their own validation, or silently process unknown frame types as if
they were valid.

## Impact

While not immediately exploitable, unknown type octets can be used to:

1. **Probe application-layer handling:** An attacker can send frames with type
   bytes `0x00`, `0x03`–`0xFF` and observe whether the server reacts
   differently (information disclosure about internal handling logic).

2. **Bypass application-layer routing:** If a handler dispatches on type byte
   (e.g., `if type == 0x01: parse_as_json()`) and does not have an `else`
   branch, an unknown type byte may silently do nothing — masking injected
   frames.

3. **Future spec confusion:** If a future protocol version defines type `0x03`,
   old servers that forward unknown types to the application will process it
   rather than rejecting it.

## Location

- `implementations/cpp/libculpeo-transport-h2/src/h2_client.cpp`, `RecvState::drain_frames` (~line 60)
- `implementations/csharp/src/CulpeoStream.Http2/Http2FrameReader.cs`, `ReadFrameAsync` (~line 35)
- `implementations/typescript/packages/culpeostream-http2/src/framing.ts`, `decodeFrame` (~line 55)

## Recommendation

Validate the type octet at the transport layer before returning the frame.
Frames with unknown type bytes should be rejected with a protocol error (closing
the stream):

**C++:**
```cpp
if (type_byte != kTypeControl && type_byte != kTypeMedia) {
    // Unknown type — treat as stream-level protocol error.
    buf.clear();
    break;
}
```

**C#:**
```csharp
if (typeOctet != 0x01 && typeOctet != 0x02)
{
    throw new CulpeoProtocolException(
        "unknown-frame-type",
        $"Received frame with unknown type octet 0x{typeOctet:X2}.");
}
```

**TypeScript:**
```typescript
if (typeOctet !== CONTROL_FRAME && typeOctet !== MEDIA_FRAME) {
  throw new RangeError(
    `CulpeoStream HTTP/2: unknown frame type octet 0x${typeOctet.toString(16).padStart(2, "0")}`,
  );
}
```
