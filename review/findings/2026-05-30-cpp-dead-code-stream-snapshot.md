# CPP-003: Dead Code in Stream Snapshot — Stale Offset Arithmetic

**Date:** 2026-05-30  
**Severity:** Medium  
**Component:** C++ — `libculpeo-session/src/session.cpp`

## Description

Line ~1000 performs `stream_snapshot.offset -= (stream->offset - stream_snapshot.offset)`
immediately after creating the snapshot. Since both values are equal at that point, this
subtracts zero and the line has no effect. The snapshot is then immediately overwritten
on line 1002. The arithmetic is dead code and may indicate a latent logic error.

## Location

`libculpeo-session/src/session.cpp`, ~lines 999–1002

## Recommendation

Remove the dead arithmetic line. Verify the intent — if pre-advance offset is needed,
capture it before calling `advance_offset`.

## Status

Open
