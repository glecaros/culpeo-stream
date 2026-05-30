# SEC-014: Content-Type Optional on Media Frames — Codec Confusion Attack

**Date:** 2026-05-30  
**Severity:** Medium  
**Component:** C++ — `libculpeo-session/src/session.cpp`

## Description

`process_media_frame` skips Content-Type validation if the header is absent
(`if (f.content_type.has_value())`). A client can omit `Content-Type` and send
payloads of any codec on a stream declared with a different codec. The server accepts
and forwards the data with the declared stream codec, causing silent data corruption
in applications that rely on the declared codec for processing.

## Recommendation

Make `Content-Type` mandatory on media frames. Return `protocol-error` if absent.

## Status

Open
