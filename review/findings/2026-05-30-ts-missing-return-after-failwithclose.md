# TS-001: Missing Return After failWithClose in handleAuthResponse (Critical)

**Date:** 2026-05-30  
**Severity:** Critical  
**Component:** TypeScript — `culpeostream/src/session.ts`

## Description

In `handleAuthResponse`, when `pendingAuthNonce === undefined`, `failWithClose` is called
but execution continues past it because there is no `return` statement. The auth-nonce
comparison on the following lines then runs against `undefined`, producing incorrect
session closure behavior.

## Location

`packages/culpeostream/src/session.ts` — `handleAuthResponse`, lines ~760–774

## Recommendation

Add `return;` after each `await this.failWithClose(...)` call in `handleAuthResponse`.

## Status

Open
