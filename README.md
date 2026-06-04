# CulpeoStream Monorepo (archived)

> **This repository has been migrated to [culpeo-labs](https://github.com/culpeo-labs).**
> It is kept for historical reference only. Please use the new repos below.

| Repo | Description |
|------|-------------|
| [culpeo-labs/culpeostream-spec](https://github.com/culpeo-labs/culpeostream-spec) | Protocol specification, security analysis, interop fixtures |
| [culpeo-labs/culpeostream-ts](https://github.com/culpeo-labs/culpeostream-ts) | TypeScript — browser + Node.js |
| [culpeo-labs/culpeostream-csharp](https://github.com/culpeo-labs/culpeostream-csharp) | C# — ASP.NET Core |
| [culpeo-labs/culpeostream-cpp](https://github.com/culpeo-labs/culpeostream-cpp) | C++ core library + Python bindings |
| [culpeo-labs/culpeo-docs](https://github.com/culpeo-labs/culpeo-docs) | Documentation site |

---

This repository contains the CulpeoStream protocol specification, reference implementations, and security analysis.

## Repository Structure

```
culpeo-stream/
  .github/
    agents/                       ← Copilot agent definitions
      cpp.agent.md
      csharp.agent.md
      typescript.agent.md
      security.agent.md
  spec/
    culpeostream-spec.md          ← protocol specification (source of truth)
  implementations/
    csharp/                       ← C# / ASP.NET Core implementation
      src/
      tests/
      samples/
      DECISIONS.md
    typescript/                   ← TypeScript browser + Node.js implementation
      packages/
      examples/
      DECISIONS.md
    cpp/                          ← C++ core library + Python bindings
      libculpeo-message/
      libculpeo-session/
      bindings/
      samples/
      DECISIONS.md
  security/
    threat-model.md               ← spec threat analysis
    findings/                     ← one file per security finding
    required-security-tests.md    ← security test cases all impls must pass
    DECISIONS.md
  interop/
    frames/                       ← golden frame fixtures all parsers must handle
    sessions/                     ← session lifecycle fixtures
    README.md                     ← how to run interop tests
  DECISIONS.md                    ← cross-cutting decisions (all agents append here)
  README.md                       ← this file
```

## Decision Logs

Every agent maintains a `DECISIONS.md` in their directory. Cross-cutting decisions (those affecting more than one implementation or the spec itself) are also appended to the root `DECISIONS.md`.

**Rule:** decisions are logged when made, not at the end of a phase. An undocumented decision discovered during review is treated as a finding.

## Interop

The `interop/` directory contains shared test fixtures that all implementations must pass:

- `frames/` — binary and text frame golden files with expected parse results
- `sessions/` — session lifecycle scenarios with expected message sequences

Each implementation is responsible for writing a test that loads and validates against these fixtures. See `interop/README.md` for the fixture format.

## Security Review

The Security Agent reviews the spec and all implementations. Their sign-off is required before any implementation moves to the next phase. Findings are filed in `security/findings/`. Critical and High findings block progress.

## Phase Sequencing

1. Security Agent produces initial threat model (`security/threat-model.md`)
2. Spec is updated to address any Critical/High findings
3. All three implementation agents begin Phase 1 in parallel
4. Security Agent reviews Phase 1 of each implementation before Phase 2 begins
5. Implementation agents continue in parallel through remaining phases
6. Interop tests are written and passing before any implementation is considered complete

## Versioning

The spec version and implementation versions are tracked independently. The spec version is declared in `spec/culpeostream-spec.md`. Each implementation declares which spec version it targets in its own README.
