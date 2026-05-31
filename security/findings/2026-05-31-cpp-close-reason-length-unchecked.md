## Finding: SEC-017 — Close Reason Length Unchecked, Non-UTF-8 Accepted
**Severity:** Medium
**Target:** C++
**Phase:** Phase 3 — `libculpeo-transport-ws`

### Description

`WsTransport::close(int code, std::string_view reason)` passes the `reason` argument
directly to the injected `close_fn_` without performing any of the three validity checks
required by RFC 6455 §5.5.1:

1. **Maximum length**: the close-frame reason payload MUST NOT exceed 123 bytes (the
   maximum payload of a WebSocket control frame is 125 bytes; 2 are consumed by the
   status code).
2. **UTF-8 encoding**: the reason MUST be valid UTF-8.
3. **Control characters**: no explicit prohibition, but non-printable bytes (including
   `\r\n`) embedded in a reason field that is echoed into application logs create a log-
   injection surface.

`uws_adapter.hpp` compounds the problem: the `reason_copy` captured inside the
`loop->defer()` lambda is handed directly to `ws->end(code, reason_copy)`. uWebSockets'
`WebSocket::end()` is documented to silently send whatever bytes are supplied; it does
not validate or truncate the reason field itself.

The test suite (`transport_ws_tests.cpp`) covers normal reasons ("Protocol Error",
"Goodbye", "Unauthorized") but includes no tests for:
- A reason longer than 123 bytes
- A reason containing `\r\n` or null bytes
- Non-UTF-8 byte sequences

### Attack Scenario

1. The session layer constructs a close reason that exceeds 123 bytes (e.g., an error
   message that includes a long path or a formatted diagnostic string).
2. `WsTransport::close()` forwards this without truncation.
3. `ws->end()` emits a WebSocket close frame whose payload exceeds RFC 6455's 125-byte
   control-frame limit.
4. Depending on the uWebSockets version:
   - The frame may be sent with an invalid length, causing the remote peer to receive a
     malformed close handshake and terminate the connection abnormally.
   - uWebSockets may silently truncate — but truncation in the middle of a multibyte
     UTF-8 sequence produces an ill-formed UTF-8 reason, which strict receivers will
     reject as a protocol error (RFC 6455 §7.1.6).

Separately, a reason containing `\r\n` that is subsequently logged (by the session layer
or application code calling `close()`) enables a log-injection attack where one log line
appears as two or more, potentially spoofing log entries.

### Impact

- **Abnormal connection termination** when the remote peer enforces RFC 6455 close-frame
  size limits, degrading the session close handshake from graceful to abrupt.
- **Log injection** when close reasons containing `\r\n` are echoed to structured or
  line-oriented logs, allowing spoofed audit entries.

### Proposed Mitigation

In `WsTransport::close()` (or in `uws_adapter.hpp`'s `close_fn` before deferring),
enforce these three invariants:

```cpp
// In transport_ws.cpp or in the uws_adapter lambda before defer():
void WsTransport::close(int code, std::string_view reason) {
    std::lock_guard<std::mutex> lock(mu_);
    // RFC 6455 §5.5.1: control frames must not exceed 125 bytes;
    // 2 bytes are the status code, leaving 123 for the reason.
    constexpr size_t kMaxReasonBytes = 123;
    if (reason.size() > kMaxReasonBytes) {
        reason = reason.substr(0, kMaxReasonBytes);
        // Back off any truncated multibyte UTF-8 sequence here if needed.
    }
    close_fn_(code, reason);
}
```

Additionally:
- Strip or replace `\r`, `\n`, and null bytes before passing the reason to the callback.
- Add test cases: reason > 123 bytes, reason containing `\r\n`, non-UTF-8 bytes.

### Spec Reference

RFC 6455 §5.5, §5.5.1 — Control Frames (maximum 125-byte payload, UTF-8 reason).
CulpeoStream spec §7.3 — Close Codes (close reasons propagated to `ITransport::close`).
