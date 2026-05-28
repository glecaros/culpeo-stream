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
