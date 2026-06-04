## Finding: Server — internal exception message sent verbatim as WebSocket 4002 close reason

**Severity:** Medium
**Target:** TypeScript
**Phase:** Phase 5 — @culpeo/async-ws adoption / server.ts

### Description

`server.ts` lines 353–355, inside `runMessageLoop`, catch every exception thrown
during frame parsing or session processing and forward the `.message` string
directly to the client as a WebSocket 4002 close reason:

```typescript
} catch (err) {
  const reason =
    err instanceof Error ? err.message.slice(0, 123) : "Protocol error";
  await this.wsClient.close(4002, reason);
  break;
}
```

This block covers two sources of exceptions:

1. `parseFrame(frameInput, ...)` — can throw on malformed input (messages are
   typically safe: "missing header terminator", "invalid UTF-8", etc.).
2. `this.coreSession.receive(frame)` — can throw from deep inside the session
   state machine, where error messages may include internal state such as stream
   IDs, session IDs, offset values, or in a worst-case future regression, header
   values that contain attacker-observed data.

The 123-byte truncation limits the per-message leakage but does not prevent it;
123 bytes is sufficient to leak a session ID, a full stream ID with a descriptive
prefix, or internal enum names.

Compare with the explicitly guarded path earlier in the same file:
```typescript
// SECURITY: use a generic reason — do not reveal why auth failed.
makeInitErrorText("unauthorized", "Authentication failed.")
```
That path redacts; this path does not.

### Attack Scenario

1. Attacker establishes a valid session (authenticated).
2. Attacker sends a deliberately malformed or out-of-sequence frame designed to
   trigger a specific code path in `coreSession.receive()`.
3. Server throws an internal error — e.g., `"stream 'audio-main' offset 98304
   precedes committed offset 102400"`.
4. The close reason `4002 stream 'audio-main' offset 98304 precedes committed
   offset 102400` is delivered to the client.
5. Attacker learns internal stream IDs, absolute offsets, and session buffer
   state — information useful for a session resumption attack.

### Impact

Information disclosure: internal session state (stream IDs, byte offsets,
state machine transitions) is leaked to the attacker.  In a future regression
where a token or nonce value appears in an exception message, this path would
escalate to credential disclosure.

### Suggested Fix

Replace the raw error forwarding with a generic client-facing message and log
the full error server-side:

```typescript
} catch (err) {
  // SECURITY: do not forward internal error details to the client.
  const logMsg = err instanceof Error ? err.message : String(err);
  console.error("[culpeostream-server] protocol error (closing 4002):", logMsg);
  await this.wsClient.close(4002, "Protocol error");
  break;
}
```

If specific client-safe error categories must be forwarded, define an explicit
allowlist of `CulpeoError` codes (e.g., `"protocol-error"`, `"unknown-stream"`)
and map them to pre-defined reason strings.

### Spec Reference

CulpeoStream spec §7 (Error Handling); close codes 4001/4002 are defined but
the spec does not require forwarding internal reason strings.
