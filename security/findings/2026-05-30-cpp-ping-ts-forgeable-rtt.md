# SEC-016: Ping Timestamp Is Predictable — RTT Measurement Forgeable

**Date:** 2026-05-30  
**Severity:** Low  
**Component:** C++ — `libculpeo-session/src/session.cpp` ping/pong

## Description

`send_ping` uses the current Unix epoch in microseconds as the ping "nonce". A
malicious client can guess the timestamp (current time ± a few ms), construct a pong
with a valid `ts`, and inject false RTT measurements. Applications using `on_rtt` for
adaptive bitrate or congestion control can be misled.

## Recommendation

Use a cryptographically random 64-bit nonce from `culpeo::crypto::secure_random` for
pong authenticity, keeping the timestamp as a separate `server_ts` field for RTT
calculation only.

## Status

Open
