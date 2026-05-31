# CPP-P3-004: ITransport Lifetime Requirement Not Documented — Dangling Reference Risk

**Date:** 2026-05-30  
**Severity:** Medium  
**Component:** C++ — `libculpeo-session/include/culpeo/session.hpp`

## Description

`Session` stores `ITransport&` (a reference member), implying the transport must
outlive the session — but this requirement is not documented in the header. The
`uws_adapter.hpp` example shows both in the same `UserData` struct with no explicit
destruction ordering, making the requirement easy to violate silently.

## Recommendation

Add `/// @param transport MUST outlive this Session.` to the constructor doc.
Add a destruction-order note to `uws_adapter.hpp` showing Session reset before
transport reset.

## Status

Open
