# CPP-001: Content-Type Comparison Fully Case-Insensitive, Violates Spec §6.2 (Critical)

**Date:** 2026-05-30  
**Severity:** Critical  
**Component:** C++ — `libculpeo-session/src/session.cpp`

## Description

The Content-Type comparison converts both strings entirely to lowercase, making parameter
*values* case-insensitive. Spec §6.2 requires case-sensitive comparison for parameter
values: `audio/pcm;rate=16000` must NOT match `audio/pcm;rate=16000` when values differ
in case. This will cause interop failures with implementations that generate mixed-case
parameter values.

## Location

`libculpeo-session/src/session.cpp`, lines ~956–964 (content-type matching)

## Recommendation

Parse type/subtype and parameters separately. Apply case-insensitive comparison to
type/subtype and parameter names; case-sensitive comparison to parameter values.

## Status

Open
