# CulpeoStream Security Threat Model — Phase 1

**Date:** 2026-05-26  
**Target:** Spec  
**Source Documents:** `.github/agents/security.agent.md`, `spec/culpeostream-spec.md`  
**Scope:** Protocol-spec threat analysis for Authentication & Authorization, Session Resumption, Frame Parsing, Denial of Service, and Transport Security.

## Finding: Bearer token exposure in protocol frame headers
**Severity:** High
**Target:** Spec
**Phase:** Part 1 — Authentication & Authorization
**Status:** Resolved
**Resolution:** Spec §A.6 now requires credential redaction from all logs, errors, traces, and telemetry. Native implementations must zero credential buffers after use.

### Description
The spec places bearer credentials in the `Authorization` header inside `culpeo.init` and `culpeo.auth-response` frames. Unlike the WebSocket HTTP upgrade, these headers live in application data that is frequently inspected, mirrored, debug-logged, or captured by WebSocket middleware, reverse proxies, observability systems, and crash tooling. The spec requires encrypted transport, but it does not require token redaction, prohibit frame logging, or recommend a safer credential carriage pattern.

### Attack Scenario
An operator enables frame-level debug logging on a reverse proxy or application gateway. `culpeo.init` and `culpeo.auth-response` frames are captured verbatim, including bearer tokens. Anyone with access to logs, traces, dumps, or packet capture inside the trusted network can replay the stolen token.

### Impact
Token theft enables unauthorized session creation, session resumption, and cross-session impersonation depending on token scope.

### Proposed Mitigation
Move authentication out of ordinary frame headers where possible, or explicitly require end-to-end redaction of `Authorization` from logs, traces, exceptions, and telemetry. Require implementations to disable frame logging by default for authenticated frames. Recommend short-lived, audience-bound tokens and prohibit credential reflection in protocol errors.

### Spec Reference
Sections 4.2, 6.1 (`culpeo.init`, `culpeo.auth-response`), A.1-A.5, B.5

## Finding: Server-driven auth refresh can be abused to harvest fresh tokens
**Severity:** High
**Target:** Spec
**Phase:** Part 1 — Authentication & Authorization
**Status:** Resolved
**Resolution:** Spec §A.4 now limits to one outstanding challenge per session, enforces a 30s minimum interval, and requires single-use nonces. §6.1 allows clients to refuse excessive challenges.

### Description
`culpeo.auth-refresh` allows the server to request a fresh token at any time. The nonce only proves liveness for a single challenge; it does not limit challenge frequency, bind refresh requests to a server-authenticated reason, or restrict how often a client must mint new tokens. A malicious or compromised server can repeatedly force token renewal and collect a stream of fresh credentials.

### Attack Scenario
A hostile endpoint establishes a valid session, then emits repeated `culpeo.auth-refresh` frames. The client, following spec, keeps sending new bearer tokens in `culpeo.auth-response`. The attacker stores each token for later use outside the live session.

### Impact
Fresh credential harvesting extends compromise beyond a single session and defeats token expiry assumptions.

### Proposed Mitigation
Allow at most one outstanding refresh challenge per session, require a minimum refresh interval, and define client refusal behavior for excessive or unjustified refreshes. Bind refresh responses to the existing session and challenge ID, and recommend proof-of-possession or token exchange that yields a session-bound token rather than a reusable bearer.

### Spec Reference
Sections 6.1 (`culpeo.auth-refresh`, `culpeo.auth-response`), A.4-A.5

## Finding: Session ID entropy requirements are underspecified
**Severity:** High
**Target:** Spec
**Phase:** Part 1 — Authentication & Authorization
**Status:** Resolved
**Resolution:** Spec §A.5 now requires a minimum of 128 bits of entropy from a CSPRNG.

### Description
The spec says session IDs must be generated with a cryptographically secure random number generator and be unguessable, but it provides no minimum entropy, encoding guidance, or length requirement. That leaves room for truncated identifiers, predictable encodings, or mixed-format implementations with materially different attack resistance.

### Attack Scenario
One implementation chooses 64-bit hex session IDs for convenience. An attacker combines brute-force guessing with resume attempts and valid stolen tokens, eventually colliding with live sessions.

### Impact
Predictable or low-entropy session IDs materially lower the cost of session hijacking and targeted resumption attacks.

### Proposed Mitigation
Specify a minimum of 128 bits of entropy, recommend unpadded base64url or equivalent safe encoding, and require constant-time comparison where session IDs are security-relevant.

### Spec Reference
Sections 4.2, 5.3, A.5

## Finding: Token scope is not defined for session ownership or resumption rights
**Severity:** High
**Target:** Spec
**Phase:** Part 1 — Authentication & Authorization
**Status:** Accepted Risk
**Resolution:** Token scope is intentionally left to the server implementation per §A.3. The risk is mitigated by mandatory TLS (§3.1) and credential redaction (§A.6), which limit the attack surface to implementation-level log leakage.

### Description
The spec requires an `Authorization` header on init and resume, but it does not define whether a token authorizes only session creation, only the authenticated principal, or a specific live session. Without explicit session-binding rules, a stolen token may be reused to resume any known session owned by the same principal or possibly any session accepted by the backend.

### Attack Scenario
An attacker steals a bearer token from logs and learns a victim session ID from client telemetry. The attacker submits a resume `culpeo.init` using that token and session ID. If the backend validates only that the token is generally valid, the attacker attaches to the victim session.

### Impact
Session ownership becomes ambiguous, enabling cross-device session takeover and broken authorization boundaries.

### Proposed Mitigation
Define that session resumption is authorized only when the resume credential is bound to the original authenticated subject and to the specific session. Recommend a session-bound resumption secret, channel binding, or proof-of-possession credential instead of a reusable bearer token.

### Spec Reference
Sections 6.1 (`culpeo.init`), 7.2, A.2-A.5

## Finding: Session resumption relies on session ID plus bearer token without additional binding
**Severity:** Medium
**Target:** Spec
**Phase:** Part 1 — Session Resumption
**Status:** Accepted Risk
**Resolution:** With mandatory TLS (§3.1) and credential redaction (§A.6), the practical attack surface requires a combined log leakage of both session ID and token. No additional resumption secret is warranted.

### Description
The current resume flow requires only `Session-Id`, stream hints, and a valid `Authorization` header. The spec does not require possession of any secret issued specifically for resumption. However, with mandatory TLS (Section 3.1), session IDs and tokens cannot be passively observed on the wire. The practical risk is limited to credential leakage through logs, crash dumps, or application telemetry — which is addressed by Section A.6's credential redaction requirements.

### Attack Scenario
A session ID leaks through telemetry or support logs alongside a valid token that was not properly redacted. An attacker uses both to resume the session.

### Impact
Session takeover if both session ID and token are leaked through a logging/redaction failure.

### Proposed Mitigation
Enforce credential redaction (A.6) and session ID confidentiality. Implementations SHOULD treat session IDs as sensitive values and avoid logging them in production. No additional resumption secret is required given mandatory TLS.

### Spec Reference
Sections 4.2, 6.1 (`culpeo.init`, `culpeo.init-ack`), 7.2, A.5, A.6

## Finding: Resumption buffer poisoning can replay attacker-controlled frames
**Severity:** High
**Target:** Spec
**Phase:** Part 1 — Session Resumption
**Status:** Open
**Resolution:** Not yet addressed in the spec. Implementations should validate frames before buffering, but normative guidance is still needed.

### Description
The server replays buffered frames after resume, but the spec does not define which frames are bufferable, who is allowed to contribute to the replay buffer, or what validation must occur before buffering. If attacker-controlled frames enter the replay set, resume can amplify a single injection into a persistent post-reconnect compromise.

### Attack Scenario
An attacker sends crafted frames on a duplex stream or exploits lax validation on application events. Those frames are accepted into the session replay buffer. After a disconnect, the victim reconnects and the server replays the poisoned frames as trusted session history.

### Impact
Replay can re-trigger malicious application events, corrupt client state, or deliver hostile media/events after reconnection.

### Proposed Mitigation
Specify that only authenticated, directionally valid, fully parsed frames may enter replay buffers. Distinguish client-originated from server-originated replay, and require implementations to buffer only the minimum replay set needed for session continuity.

### Spec Reference
Sections 7.2, 8, 9

## Finding: Resume offset manipulation can trigger replay amplification and state desynchronization
**Severity:** High
**Target:** Spec
**Phase:** Part 1 — Session Resumption
**Status:** Partially Resolved
**Resolution:** Buffer-Window capped at 30s (§7.4.1) limits maximum replay scope. Offset bounds checking and per-resume replay caps are not yet specified.

### Description
Clients may supply arbitrary `resume_offset` values. The spec says the server resumes from the earliest available offset if data was partially evicted, but it does not define rejection thresholds, bounds checking, per-stream replay limits, or how future offsets are handled.

### Attack Scenario
A client repeatedly reconnects with very old offsets to force maximum replay work and memory retention, or with far-future offsets to skip state unexpectedly and provoke error-heavy recovery paths.

### Impact
Replay amplification increases CPU, I/O, and bandwidth costs; malformed future offsets can create inconsistent stream state or repeated resume failures.

### Proposed Mitigation
Require servers to validate offsets against `[earliest_buffered, latest_seen + tolerance]`, reject impossible future offsets, and cap replay bytes/frames per resume attempt. Allow servers to fail resume instead of honoring pathological offsets.

### Spec Reference
Sections 6.1 (`culpeo.init`, `culpeo.init-ack`), 7.2, 8.2

## Finding: Header value injection is not explicitly forbidden
**Severity:** High
**Target:** Spec
**Phase:** Part 1 — Frame Parsing
**Status:** Resolved
**Resolution:** Spec §4.1 now requires rejection of CR, LF, and NUL in header names and values. §4.1 also requires rejection of duplicate headers as a protocol error.

### Description
The protocol inherits an HTTP-like header block but does not explicitly say implementations must reject CR, LF, or NUL in header names and values after transport decoding. Without an explicit rejection rule, parsers may normalize or split attacker-controlled values into additional headers.

### Attack Scenario
A malicious peer includes `\r\nAuthorization: Bearer attacker` inside a header value. A permissive parser or downstream logger interprets the embedded delimiter as a new header line, altering frame semantics or confusing audit trails.

### Impact
Header smuggling can corrupt parser state, bypass validation, or poison logs and security tooling.

### Proposed Mitigation
Require parsers to reject any header containing CR, LF, or NUL in the field name or value, reject empty field names, and reject duplicate reserved headers.

### Spec Reference
Sections 4, 4.1, 4.2

## Finding: No maximum header block size creates trivial memory-exhaustion attacks
**Severity:** High
**Target:** Spec
**Phase:** Part 1 — Frame Parsing
**Status:** Resolved
**Resolution:** Spec §4.1.1 defines mandatory parser limits: 8KB max header block, 64 max headers, 256B max name, 4KB max value. Configurable higher but never unlimited.

### Description
The frame format relies on `\r\n\r\n` as a terminator, but the spec defines no maximum header block size, maximum header count, or maximum individual header length. Implementations can be forced to buffer unbounded data while searching for the terminator.

### Attack Scenario
An attacker opens connections and sends giant text or binary frames containing megabytes or gigabytes of header-like data without a terminator, forcing allocations and parser work until the process is unstable.

### Impact
A single client can trigger memory pressure, allocator churn, and process termination.

### Proposed Mitigation
Define mandatory parser limits for total header block size, header count, and per-header size, and require rejection before additional buffering once the limit is exceeded.

### Spec Reference
Sections 4, 4.1, B.2-B.3

## Finding: Content-Type confusion can drive unsafe decoder selection
**Severity:** Medium
**Target:** Spec
**Phase:** Part 1 — Frame Parsing
**Status:** Open
**Resolution:** Not yet addressed in the spec. Per-frame Content-Type validation against the declared stream type is recommended but not normatively required.

### Description
Streams declare a `content_type`, and media frames also carry `Content-Type`, but the spec does not require strict equality or semantic validation on every frame. If an implementation trusts the stream declaration or the per-frame header inconsistently, an attacker can steer data into the wrong decoder.

### Attack Scenario
A stream declared as PCM begins carrying Opus or malformed binary while still labeled as PCM, or vice versa. A decoder chosen using the wrong assumption crashes, over-allocates, or passes corrupted data to upper layers.

### Impact
Decoder misuse can cause denial of service, memory safety exposure in native code, or application-level confusion.

### Proposed Mitigation
Require per-frame validation that the frame `Content-Type` exactly matches the stream's negotiated media type, including critical parameters for PCM.

### Spec Reference
Sections 5.2, 5.5, 6.2, 8.2

## Finding: Reserved event namespace enforcement is vulnerable to normalization ambiguities
**Severity:** Medium
**Target:** Spec
**Phase:** Part 1 — Frame Parsing
**Status:** Resolved
**Resolution:** Spec §9.5 defines event names as case-sensitive ASCII with no normalization. Malformed `culpeo.*` variants must be rejected as protocol errors. §10.2 clarifies that syntax validation (§9.5) runs before unknown-event handling.

### Description
The spec reserves `culpeo.*` but relies on string matching without defining canonicalization rules. Mixed case, extra dots, trailing spaces, or other non-canonical forms can lead to inconsistent classification between implementations.

### Attack Scenario
One implementation treats `CULPEO.init` as an application event while another lowercases and handles it as a protocol event. Cross-implementation behavior diverges, enabling bypasses and downgrade-like confusion.

### Impact
Inconsistent namespace handling weakens protocol invariants and can bypass reserved-event enforcement.

### Proposed Mitigation
Define event names as case-sensitive ASCII tokens with no leading/trailing whitespace, no empty labels, and an exact reserved prefix rule for `culpeo.`. Require rejection, not normalization, for malformed reserved names.

### Spec Reference
Sections 6.1, 9.1, 9.4, 10.2

## Finding: Unlimited stream declaration enables resource exhaustion at session start
**Severity:** High
**Target:** Spec
**Phase:** Part 1 — Denial of Service
**Status:** Resolved
**Resolution:** Spec §5.6 caps streams at 16 per session by default. Server must reject excess before allocating per-stream resources.

### Description
The spec requires at least one stream but does not cap the number of declared streams. `culpeo.init` can therefore demand arbitrary per-stream allocation, validation, bookkeeping, and replay state.

### Attack Scenario
A client submits an init frame with 10,000 stream objects and large unique `purpose` values. The server spends CPU and memory validating, mapping, and retaining them.

### Impact
Session establishment becomes an inexpensive attack vector for CPU and memory exhaustion.

### Proposed Mitigation
Specify a protocol maximum stream count and allow servers to advertise or enforce lower implementation-specific limits with a dedicated error code.

### Spec Reference
Sections 5, 5.5, 6.1 (`culpeo.init`)

## Finding: Ping flooding can force unbounded responder work
**Severity:** Medium
**Target:** Spec
**Phase:** Part 1 — Denial of Service
**Status:** Resolved
**Resolution:** Spec §6.1 (`culpeo.ping`) now rate-limits to 5/sec per session. Excess pings are silently dropped without closing the session.

### Description
The spec says receivers MUST respond to `culpeo.ping`, but it sets no rate limit, no aggregation rule, and no abuse handling. Attackers can force a reply for every inbound ping.

### Attack Scenario
A client floods the server with pings at line rate. The server, following spec, emits a pong for each one and burns CPU and bandwidth on useless work.

### Impact
Amplified responder work can starve legitimate media processing and degrade shared infrastructure.

### Proposed Mitigation
Require per-session ping rate limits and allow dropping or coalescing excessive pings without closing healthy sessions immediately.

### Spec Reference
Sections 6.1 (`culpeo.ping`, `culpeo.pong`), B.4

## Finding: Buffer-window negotiation lacks mandatory upper bounds
**Severity:** High
**Target:** Spec
**Phase:** Part 1 — Denial of Service
**Status:** Resolved
**Resolution:** Spec §7.4.1 caps Buffer-Window at 30s by default. Server must clamp before allocating and reflect the actual value in init-ack.

### Description
Clients may request arbitrary `Buffer-Window` values and servers may reduce them, but the spec defines no mandatory ceiling and no replay-size accounting. A permissive server can be coerced into retaining excessive buffered media.

### Attack Scenario
An attacker requests a massive buffer window on many sessions, then disconnects and reconnects repeatedly to force retention and replay of large media histories.

### Impact
Memory exhaustion and replay amplification threaten overall service availability.

### Proposed Mitigation
Define a mandatory maximum buffer window, recommend byte-based replay limits in addition to time-based limits, and permit servers to reject requests that exceed policy.

### Spec Reference
Sections 4.2, 6.1 (`culpeo.init`, `culpeo.init-ack`), 7.4

## Finding: Reconnection storms are not handled as a security-sensitive load condition
**Severity:** Medium
**Target:** Spec
**Phase:** Part 1 — Denial of Service
**Status:** Open
**Resolution:** Buffer-Window cap (§7.4.1) limits per-session replay cost, but the spec still lacks normative guidance on jittered backoff, overload responses, or admission control during recovery.

### Description
The spec says clients MAY use exponential backoff, but it does not give stronger guidance for coordinated reconnects after restart or network flap. Because resume can trigger buffered replay, reconnect storms can become disproportionately expensive.

### Attack Scenario
After a server restart, thousands of clients reconnect immediately and request replay. An attacker can intentionally induce the same pattern by causing repeated disconnects.

### Impact
Availability drops during recovery, exactly when session continuity is most needed.

### Proposed Mitigation
Strengthen client guidance to SHOULD use jittered exponential backoff, allow servers to issue retry hints or overload responses, and cap replay work during degraded mode.

### Spec Reference
Sections 7.2, 7.4, 10.4

## Finding: Transport downgrade and proxy interference are insufficiently detectable
**Severity:** High
**Target:** Spec
**Phase:** Part 1 — Transport Security
**Status:** Open
**Resolution:** Sub-protocol negotiation is now mandatory (§B.1), but trusted proxy / internal-hop verification guidance is not yet specified.

### Description
The spec requires `wss://` in production, but it does not define how clients or servers detect downgrade through proxy misconfiguration, TLS termination gaps, or stripped security metadata. A deployment can appear compliant while exposing plaintext on an internal hop.

### Attack Scenario
A load balancer terminates TLS and forwards plain `ws://` to an internal service without conveying trustworthy security context. Operators assume end-to-end security, while internal observers can read tokens and media.

### Impact
Confidentiality of credentials and streamed content is lost despite nominal use of WebSockets.

### Proposed Mitigation
Require implementations in production to verify transport security at the endpoint actually processing frames, define trusted proxy signaling requirements, and fail closed when secure transport cannot be confirmed.

### Spec Reference
Sections 3.1, A.5, B.5

## Finding: Sub-protocol spoofing allows non-compliant endpoints to look acceptable
**Severity:** Medium
**Target:** Spec
**Phase:** Part 1 — Transport Security
**Status:** Resolved
**Resolution:** Spec §B.1 upgraded sub-protocol negotiation from SHOULD to MUST. Client must verify server echoes the sub-protocol before sending any frames.

### Description
The WebSocket binding only says the `culpeostream` sub-protocol SHOULD be declared. A server that ignores `Sec-WebSocket-Protocol` may still accept the connection, leaving the client to discover incompatibility only after sending sensitive frames.

### Attack Scenario
A client connects to a generic WebSocket endpoint that accepts upgrades without negotiating `culpeostream`. The client sends `culpeo.init` containing credentials before realizing the peer is not actually a CulpeoStream server.

### Impact
Credentials and session metadata can be disclosed to the wrong service, and downgrade/endpoint confusion becomes easier.

### Proposed Mitigation
Make sub-protocol negotiation mandatory for the WebSocket binding and require an explicit server-side protocol confirmation in `culpeo.init-ack`, such as the accepted protocol version and binding identifier.

### Spec Reference
Sections 6.1 (`culpeo.init-ack`), B.1-B.3

---

## Top 5 Spec Issues to Resolve Before Stability

1. ~~**Define mandatory parser limits and rejection rules**~~ — ✅ Resolved in §4.1, §4.1.1
2. ~~**Require credential redaction**~~ — ✅ Resolved in §A.6
3. ~~**Constrain auth-refresh abuse**~~ — ✅ Resolved in §A.4, §6.1
4. ~~**Define hard resource caps**~~ — ✅ Resolved in §5.6, §6.1, §7.4.1
5. ~~**Strengthen transport binding requirements**~~ — ✅ Resolved in §B.1
