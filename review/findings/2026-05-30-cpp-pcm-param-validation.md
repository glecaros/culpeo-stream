# CPP-002: PCM Content-Type Parser Does Not Validate channels≥1 or bits%8==0

**Date:** 2026-05-30  
**Severity:** High  
**Component:** C++ — `libculpeo-message/src/message.cpp`

## Description

The PCM content-type parser accepts `channels=0` and non-multiple-of-8 `bits` values
(e.g., `bits=3`). Spec §6.2 mandates `channels MUST be ≥ 1` and
`bits MUST be a positive multiple of 8`. Invalid values reach the session layer and
offset arithmetic, causing undefined behaviour.

## Location

`libculpeo-message/src/message.cpp`, `parse_content_type`, ~lines 342–347

## Recommendation

After parsing parameters, add:
```cpp
if (channels == 0) return std::unexpected(Error::invalid_content_type);
if (bits == 0 || bits % 8 != 0) return std::unexpected(Error::invalid_content_type);
```

## Status

Open
