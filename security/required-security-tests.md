# CulpeoStream Required Security Tests — Phase 1

**Date:** 2026-05-26  
**Applies To:** C#, TypeScript, and C++ implementations  
**Purpose:** Prose security tests that every implementation must satisfy before the protocol is considered stable. Where a test refers to client or server behavior, it applies to any implementation that provides that role.

## Authentication & Authorization

### AUTH-01 — Authorization data must never be exposed through logs or errors
**Procedure:** Establish a session and perform at least one `culpeo.auth-refresh` / `culpeo.auth-response` exchange using a distinctive bearer token value. Enable normal application logging, structured error reporting, and protocol error handling.

**Expected Result:** The token value never appears in logs, exception messages, telemetry tags, trace attributes, or protocol error bodies. Redacted placeholders are acceptable.

### AUTH-02 — Refresh nonces must be single-use and session-bound
**Procedure:** Complete a valid `culpeo.auth-refresh` challenge, then replay the same `culpeo.auth-response` frame or reuse the same nonce on a second response.

**Expected Result:** The implementation rejects the replayed response, treats the nonce as spent, and does not extend session authorization.

### AUTH-03 — Excessive refresh challenges must be refused or rate-limited
**Procedure:** Send repeated `culpeo.auth-refresh` challenges in rapid succession on a live session, including overlapping outstanding challenges.

**Expected Result:** The implementation permits at most one outstanding refresh challenge per session and rejects, drops, or rate-limits abusive refresh behavior.

### AUTH-04 — Session resumption must fail when credentials are not bound to the original session owner
**Procedure:** Create a session as principal A, then attempt to resume it with a different credential, a different session-binding secret, or a token representing principal B.

**Expected Result:** Resumption is rejected with an authorization/session error and no buffered frames are replayed.

### AUTH-05 — Session identifiers must meet minimum unpredictability requirements
**Procedure:** Generate a large sample of session IDs under test conditions and inspect their format and uniqueness.

**Expected Result:** Session IDs meet the spec's minimum entropy and encoding requirements, show no deterministic prefix/counter behavior that reduces effective entropy, and do not collide in the sample.

## Session Resumption

### RESUME-01 — Resume with a leaked session ID alone must not succeed
**Procedure:** Attempt session resumption using a valid `Session-Id` but without the required session-bound resumption secret or proof.

**Expected Result:** The implementation rejects the resume attempt and does not attach the new connection to the existing session.

### RESUME-02 — Invalid past and future resume offsets must be bounded
**Procedure:** Attempt resume with per-stream `resume_offset` values that are far behind the earliest buffered offset, slightly ahead of the latest valid offset, and extremely far in the future.

**Expected Result:** The implementation clamps only within clearly defined safe bounds or rejects the resume attempt outright. It must not allocate excessive memory, enter long replay loops, or return inconsistent offsets.

### RESUME-03 — Replay buffers must not contain unauthenticated or invalid frames
**Procedure:** Try to inject malformed, directionally invalid, or unauthorized frames into any replayable stream, then disconnect and resume.

**Expected Result:** Invalid frames never enter the replay buffer, and resumption replays only frames that previously passed full validation and authorization checks.

### RESUME-04 — Replay work must be capped per resume attempt
**Procedure:** Resume a session with the oldest still-buffered offsets on all streams repeatedly.

**Expected Result:** The implementation enforces configured replay byte/frame/time limits and remains available under repeated replay-heavy resumes.

## Frame Parsing

### PARSE-01 — Header value injection must be rejected
**Procedure:** Send frames whose header names or values contain embedded `\r`, `\n`, NUL, empty header names, or malformed duplicate reserved headers.

**Expected Result:** The parser rejects the frame as a protocol error before any downstream processing or logging treats the injected bytes as separate headers.

### PARSE-02 — Header block size and header count limits must be enforced before buffering indefinitely
**Procedure:** Send frames with an oversized header block, a very large number of headers, and a missing `\r\n\r\n` terminator.

**Expected Result:** The implementation stops buffering once limits are reached, fails the frame/connection predictably, and does not exhibit unbounded memory growth.

### PARSE-03 — Event namespace normalization bypasses must fail closed
**Procedure:** Send event names such as `CULPEO.init`, `culpeo.init `, ` culpeo.init`, `culpeo..init`, and other non-canonical reserved-prefix variants.

**Expected Result:** Malformed reserved-event forms are rejected or treated as invalid according to the hardened spec rules; they are not silently normalized into protocol events or accepted as valid application events.

### PARSE-04 — Media `Content-Type` must match the negotiated stream type on every frame
**Procedure:** Negotiate a stream as PCM, then send Opus or AAC data on it; repeat by negotiating Opus and sending PCM-labeled payloads or mismatched PCM parameters.

**Expected Result:** Every mismatch is rejected, the wrong decoder is never selected, and session handling remains stable.

### PARSE-05 — Unknown but well-formed headers must not override reserved semantics
**Procedure:** Send frames with mixed-case duplicates or near-collision names such as `authorization`, `Authorization `, `Stream-Id`, and `stream-id` in combinations intended to confuse parsers.

**Expected Result:** Reserved header handling is canonical and unambiguous; malformed or duplicate reserved headers are rejected.

## Denial of Service

### DOS-01 — Stream count limits must be enforced during `culpeo.init`
**Procedure:** Attempt initialization with stream counts at, below, and far above the configured maximum.

**Expected Result:** Sessions above the limit are rejected quickly with a deterministic error and without material allocation proportional to the full attacker-supplied stream list.

### DOS-02 — Buffer-window requests above policy must be reduced or rejected
**Procedure:** Request progressively larger `Buffer-Window` values, including extremely large values across many concurrent sessions.

**Expected Result:** The implementation never exceeds policy, reflects the actual negotiated value, and remains stable under abusive requests.

### DOS-03 — Ping floods must not force one-for-one reply amplification
**Procedure:** Send `culpeo.ping` frames at rates above normal operating levels on established sessions.

**Expected Result:** The implementation rate-limits, drops, or coalesces excess pings while preserving service for legitimate traffic.

### DOS-04 — Reconnection storms must degrade gracefully
**Procedure:** Simulate many clients reconnecting simultaneously after a disconnect or restart, all attempting buffered resumption.

**Expected Result:** The implementation uses backoff, admission control, overload signaling, replay caps, or equivalent controls to remain available and recover predictably.

## Transport Security

### TRANS-01 — Production mode must reject insecure transport
**Procedure:** Attempt to establish a production-mode session over insecure transport or through a deployment path where the endpoint processing CulpeoStream frames cannot verify that transport security was preserved.

**Expected Result:** The connection is rejected before credentials or media are accepted.

### TRANS-02 — WebSocket sub-protocol negotiation must be explicit
**Procedure:** Connect to a WebSocket peer that does not negotiate `Sec-WebSocket-Protocol: culpeostream`, or negotiates a different sub-protocol while otherwise accepting frames.

**Expected Result:** The implementation fails the handshake or aborts before sending sensitive protocol data.

### TRANS-03 — Protocol confirmation must be validated before session use
**Procedure:** Establish a connection to a peer that accepts the transport but omits or lies about protocol confirmation in the initial CulpeoStream handshake.

**Expected Result:** The implementation aborts the session instead of proceeding with an ambiguous or spoofed endpoint.
