# Security Decision Log

## Treating resume hijacking as the top blocker
**Date:** 2026-05-26
**Target:** Spec
**Status:** Resolved

### Context
Phase 1 required a threat model of the protocol spec before implementation review begins.

### Finding or Decision
I treated session resumption without a session-bound secret or proof-of-possession mechanism as the highest-priority issue because it enables direct session takeover if a session ID and acceptable credential are exposed.

### Recommended Action
Add a dedicated resumption credential bound to the original session and authenticated principal, and require it on every resume attempt.

### Resolution
Resolved. Downgraded from Critical to Medium — TLS provides sufficient transport-level protection. Spec hardened with §A.5 (128-bit minimum entropy for session IDs) and §7.2 (all-or-nothing resume stream matching, wall-clock expiry). All three implementations enforce these requirements.

## Separating token leakage from refresh abuse
**Date:** 2026-05-26
**Target:** Spec
**Status:** Resolved

### Context
The authentication review surfaced two related but distinct token risks: passive disclosure through frame handling and active harvesting through repeated refresh requests.

### Finding or Decision
I recorded these as separate findings instead of collapsing them into one auth issue. They have different attack paths, different mitigations, and different implementation tests.

### Recommended Action
Address both with independent spec changes: one set for redaction/logging and one set for refresh semantics and rate limits.

### Resolution
Resolved. Token leakage addressed by §A.6 (credential confidentiality — MUST NOT log/expose Authorization values). Refresh abuse addressed by §A.4 (single-use nonces, one outstanding challenge, 30s minimum interval, mandatory timeout). All three implementations enforce both.

## Requiring explicit parser hardening requirements
**Date:** 2026-05-26
**Target:** Spec
**Status:** Resolved

### Context
The frame format is intentionally HTTP-like, but the current draft leaves several parser behaviors undefined.

### Finding or Decision
I treated header injection, duplicate reserved headers, and unbounded header buffering as spec defects rather than implementation details because inconsistent parser behavior across languages creates direct security drift.

### Recommended Action
Define normative rejection rules for CR/LF/NUL, empty names, duplicate reserved headers, maximum header block size, maximum header count, and maximum header length.

### Resolution
Resolved. Spec hardened with §4.1 (CR/LF/NUL rejection, duplicate header rejection, header ordering insignificance), §4.1.1 (mandatory parser limits with configurable defaults), and §4.1 OWS rules. All three implementations enforce identical limits (8KB block, 64 headers, 256B name, 4KB value) with matching validation. Cross-implementation divergence review confirmed consistent behavior.

## Accepting `ws://` only as a constrained development risk
**Date:** 2026-05-26
**Target:** Spec
**Status:** Accepted Risk

### Context
The spec permits unencrypted transport in local development environments.

### Finding or Decision
I accepted this only as a narrowly scoped development risk. It is not acceptable in production, staging environments with real credentials, or any deployment where transport security at the frame-processing endpoint is ambiguous.

### Recommended Action
Keep the development carve-out, but require production implementations to fail closed when secure transport cannot be verified and to document trusted proxy assumptions.

### Resolution
Accepted Risk for local-only development. Production guidance still requires tightening.

## Phase 1 implementation review — scope and methodology
**Date:** 2026-05-28
**Target:** C# | TypeScript | C++
**Status:** Resolved

### Context
Reviewing all three Phase 1 implementations against the Part 2 checklists in the security agent instructions. Phase 2 is beginning in parallel.

### Finding or Decision
Reviewed all source files directly rather than delegating to sub-agents because the relevant files are few, well-bounded, and reading them in context produces higher-confidence findings than summarized output. This kept the review window clean and all tracing auditable in this log.

### Recommended Action
File findings immediately so Phase 2 agents see them before writing new code.

### Resolution
Resolved. Seven findings filed to `security/findings/`. See individual files for details.

---

## Accepting silent-drop as valid alternative to close for excess pings
**Date:** 2026-05-28
**Target:** C# | TypeScript
**Status:** Open

### Context
Both C# and TypeScript DECISIONS.md claim the connection is closed with `rate-limit-exceeded` on ping flood, but the code silently drops excess pings. Both behaviors are defensible.

### Finding or Decision
I treated this as a Medium finding rather than High because:
1. The silent-drop behavior is not weaker from a DoS perspective — it avoids reconnection storms.
2. The failure is documentation vs. implementation divergence, not an active security gap.
3. No monitoring code relying on `rate-limit-exceeded` exists in Phase 1.

### Recommended Action
The implementations must explicitly choose one behavior, document it correctly, and both must match. The current state (both drop silently, both document close) is a contract violation that can silently break monitoring code added in later phases.

### Resolution
Open. Awaiting decision from C# and TypeScript agents.

---

## Accepting TypeScript JSON.parse prototype pollution risk as Low
**Date:** 2026-05-28
**Target:** TypeScript
**Status:** Accepted Risk

### Context
`parseJsonObject` uses `JSON.parse` on attacker-controlled frame bodies. Prototype pollution via `__proto__` keys in JSON is a known JavaScript vulnerability class.

### Finding or Decision
Modern JavaScript engines (V8, SpiderMonkey, JavaScriptCore) treat `JSON.parse` as using `[[DefineOwnProperty]]` semantics, not `[[Set]]`. The `__proto__` setter is not triggered; any `__proto__` key becomes an own property of the parsed object, not a prototype modification. Downstream property accesses (`frame.body.nonce`, `frame.body.streams`, etc.) operate on named own properties and are not affected.

Additionally, no code path spreads a parsed body object into a shared singleton or mutable prototype.

### Recommended Action
Accepted. No action required for Phase 1. If the implementation later processes body objects with `Object.assign` or similar merges that could trigger the setter, revisit this.

### Resolution
Accepted Risk. Will monitor in Phase 2 if new body processing patterns emerge.

---

## Limiting Phase 1 scope to spec deliverables
**Date:** 2026-05-26
**Target:** Spec
**Status:** Resolved

### Context
The agent instructions also define continuous review of the C#, TypeScript, and C++ implementations.

### Finding or Decision
For this task I limited deliverables to the requested Phase 1 spec artifacts: threat model, required security tests, and the decision log. I did not create implementation findings because no implementation review was requested in this run.

### Recommended Action
Use `security/required-security-tests.md` as the gate for later implementation reviews and file implementation findings under `security/findings/` when those phases are reviewed.

### Resolution
Resolved for this task. `security/findings/` was created for future reports.

---

## Phase 3 review: session-ownership binding elevated to High
**Date:** 2026-05-31
**Target:** TypeScript
**Status:** Open

### Context
Reviewing the `culpeostream-server` Phase 3 additions, specifically the `handleInitMessage`
flow that performs authentication and then separately loads a session snapshot by ID.

### Finding or Decision
I elevated SEC-020 (session resumption with no auth-to-session binding) to **High** rather
than Medium. Although session IDs are high-entropy (UUID v4 = 122 bits) making brute-force
impractical, the authentication API design (`authenticate(authorization: string)`) positively
prevents server implementors from enforcing ownership at the authentication layer without
architectural changes. The vulnerability requires only a leaked session ID — a plausible event
via log leakage, ws:// downgrade (SEC-001), or an administrative interface — and any valid
token.

### Recommended Action
Extend `authenticate` signature to receive `sessionId?: string`, allowing server code to
enforce binding. Document that the current design relies on session ID secrecy as sole
ownership signal (which is insufficient for a defence-in-depth posture).

### Resolution
Open — awaiting TypeScript agent response. Blocks Phase 4 hardening.

---

## Phase 3 review: static token reuse on reconnect filed as Medium (not High)
**Date:** 2026-05-31
**Target:** C#
**Status:** Open

### Context
`CulpeoStreamClient` always uses `_options.Authorization` (static construction-time token)
for every `culpeo.init`, including reconnects. `GetToken` is only called reactively.

### Finding or Decision
Filed as **Medium** (SEC-018) rather than High. The primary impact is availability (reconnect
storm when token expires during disconnect), not direct credential exposure. Confidentiality
is not directly compromised because the token is sent over TLS. However, the design is
counter-intuitive and causes unnecessary token lifetime extension in memory.

### Recommended Action
`GetToken`, when provided, should be called before every `culpeo.init`. See SEC-018.

### Resolution
Open — awaiting C# agent response. Must fix before GA.

---

## Phase 3 review: close-reason injection downgraded from High to Medium for C++
**Date:** 2026-05-31
**Target:** C++
**Status:** Open

### Context
`WsTransport::close()` passes `reason` to `ws->end()` without length or UTF-8 validation.
Initial assessment considered HTTP header injection, but WebSocket close frames are binary
frames (post-HTTP-upgrade), so `\r\n` in the reason does not inject HTTP headers.

### Finding or Decision
Filed as **Medium** (SEC-017). The real risks are: (1) RFC 6455 violation from reasons >123
bytes causing malformed close frames; (2) log injection from `\r\n` in reasons. Not Critical
or High because exploitation requires the session layer to supply an oversized or binary-
containing reason, which is an unusual but not impossible code path.

### Recommended Action
Enforce ≤123 bytes and strip `\r\n`/null bytes in `WsTransport::close()` before forwarding.
Add test cases for oversized and control-character-containing reasons.

### Resolution
Open — awaiting C++ agent response.

## Phase 4 HTTP/2 transport review scope and methodology
**Date:** 2026-06-01
**Target:** C++ | C# | TypeScript
**Status:** Resolved

### Context
Phase 4 adds HTTP/2 transport implementations across all three languages (Addendum C).
Reviewed all new Phase 4 source files directly: C++ `libculpeo-transport-h2/`, C#
`CulpeoStream.Http2/`, TypeScript `culpeostream-http2/src/`.  Also read all three
implementations' DECISIONS.md files and the Phase 4 entries in the security decisions log.

### Finding or Decision
Reviewed all Phase 4 files directly (11 source files) rather than delegating to sub-agents.
The scope is bounded and high-confidence direct review of C/C++ memory safety, C# arithmetic,
and TypeScript event-handler error propagation requires reading the actual code rather than
summaries.

### Recommended Action
File individual findings per the findings template.  Prioritize: SEC-023 (Critical TLS bypass),
SEC-024 (High C# integer overflow), SEC-025 (High unbounded stream creation), SEC-026 (High
TS client process crash).

### Resolution
Resolved — 11 findings filed (SEC-023 through SEC-033).

## Accepting `verify_none` in C++ TLS client as a blocker
**Date:** 2026-06-01
**Target:** C++
**Status:** Open

### Context
`h2_client.cpp` line 602 overrides the caller's TLS context with `verify_none`.
The comment says "tests — no CA" but this is in the production TLS path, not a
test fixture.  The `AllowCleartext` path is the correct mechanism for tests.

### Finding or Decision
Filed as **Critical** (SEC-023).  A hardcoded `verify_none` in the production TLS path
fully defeats transport security — it is not a defense-in-depth gap but a complete bypass.
This blocks all other Phase 4 security properties because the token secrecy and session
integrity claims all depend on TLS being properly terminated.

### Recommended Action
Remove `set_verify_mode(verify_none)` from `connect()`.  The caller controls the SSL
context and MUST set their own verify mode.  For tests, pass a context with `verify_none`
explicitly from the test fixture.  Do not set verify mode inside the library.

### Resolution
Open — awaiting C++ agent fix.

## C# signed-cast truncation: treating as High not Critical
**Date:** 2026-06-01
**Target:** C#
**Status:** Open

### Context
`Http2FrameReader.ReadFrameAsync` casts `uint` to `int` before comparing to `maxPayloadBytes`.
Values ≥ 0x80000000 yield negative `int`, bypass the check, then throw `OverflowException`
during `new byte[payloadLength]`.

### Finding or Decision
Filed as **High** (SEC-024) rather than Critical.  The effect is per-connection DoS
(unhandled exception terminates the handler task) rather than memory exhaustion across the
process — each connection runs in an independent task, so the server continues serving other
clients.  However, unauthenticated clients can reliably terminate their own connections before
auth is checked, which could be used to exhaust connection slots if combined with a connection
flood.

### Recommended Action
Read the length as `uint`, validate against `(uint)maxPayloadBytes`, then cast to `int`
after the check.  One-line fix.

### Resolution
Open — awaiting C# agent fix.
