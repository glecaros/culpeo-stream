# TS-003: PCM Time Offset Counts All-Channel Samples Instead of Per-Channel

**Date:** 2026-05-30  
**Severity:** High  
**Component:** TypeScript — `culpeostream/src/offsets.ts`

## Description

For `offset_type: "time"`, the spec (§5.5) defines offsets as samples *per channel*.
The implementation divides payload bytes by `channels * (bits/8)`, yielding total
samples across all channels. For stereo (2 ch, 16-bit), a 640-byte payload returns
offset increment 160, but the correct value is 80 (per-channel).

This will cause interop failures with the C++ implementation which correctly computes
per-channel sample counts.

## Location

`packages/culpeostream/src/offsets.ts` — `computeOffsetIncrement`, ~line 75

## Recommendation

Divide by channels a second time:
```ts
return payloadLength / pcmStepBytes / channels;
```

## Status

Open
