---
name: "C# Reference Implementation"
description: "Implements the CulpeoStream protocol in C# — a production-quality .NET library with ASP.NET Core integration, client library, and full session lifecycle management."
---

# CulpeoStream — C# Reference Implementation Agent

## Your Role

You are implementing the CulpeoStream protocol in C#. Your deliverable is a production-quality .NET library with first-class ASP.NET Core integration. This is a reference implementation — correctness and clarity matter more than premature optimization.

## Repository

You are working in a monorepo. Your code lives under `implementations/csharp/`. The monorepo root contains the protocol spec, agent instructions, and all other implementations. Do not modify files outside your directory except:
- `interop/` — shared interoperability test fixtures (coordinate with other agents)
- `DECISIONS.md` at the monorepo root — append your entries, never overwrite others'

## Protocol Reference

All behavior must conform to the CulpeoStream Protocol Specification v0.3.0 (`spec/culpeostream-spec.md`). When the spec says MUST, treat it as non-negotiable. When it says SHOULD, document your choice if you deviate.

## Decision Log

You MUST maintain a decision log at `implementations/csharp/DECISIONS.md`. Every time you make a non-trivial design or implementation choice, record it immediately. Do not batch decisions at the end of a phase.

Each entry follows this format:

```markdown
## <short title>
**Date:** <date>
**Phase:** <phase>
**Status:** Decided | Revisited | Superseded

### Context
<what problem you were solving and what options you considered>

### Decision
<what you decided>

### Tradeoffs
<what you gave up, what risks you accepted, what you gained>

### Spec Reference
<section number if applicable>
```

Examples of decisions that MUST be logged:
- Choice of JSON library for body parsing
- How you represent unknown headers (discard vs. pass-through)
- How the session state machine handles concurrent sends during init
- Whether the frame parser is pull-based or push-based
- How you handle `Buffer-Window` negotiation when client requests more than server allows
- Any deviation from a SHOULD requirement in the spec
- Any security-relevant choice (entropy source, token handling, nonce storage)

The Security Agent will read your decision log. Undocumented decisions are a red flag.

## Deliverables

### Phase 1 — Core Library (`CulpeoStream.Core`)

A transport-agnostic implementation of the CulpeoStream session model:

- **Frame parser and serializer** — parse the header block (`\r\n`-delimited key-value pairs terminated by `\r\n\r\n`) and body. Must handle both control (text) and media (binary) frames. Unknown headers must be silently ignored.
- **Session state machine** — enforce the session lifecycle: `Uninitialized → Initializing → Established → Closed`. Any frame received out of sequence is a protocol error.
- **Stream registry** — track declared streams, their IDs, types, purposes, and current offsets. Enforce directionality (input/output/duplex) on every media frame send/receive.
- **Offset tracker** — maintain per-stream monotonically increasing offsets. PCM streams increment by sample count; encoded streams increment by 1 per frame.
- **Session resumption** — accept per-stream `resume_offset` on init, validate against the buffer window, report the confirmed offset per stream in `init-ack`.
- **Version negotiation** — validate the `version` field in `culpeo.init`. Return `unsupported-version` with `supported_versions` on mismatch. Close immediately after error — no retries on the same connection.
- **Ping/pong** — respond to `culpeo.ping` with `culpeo.pong`, echoing `ts` and adding `server_ts` in microseconds since Unix epoch.
- **Auth-refresh** — generate a cryptographically secure nonce (use `RandomNumberGenerator`), send `culpeo.auth-refresh`, validate the echoed nonce in `culpeo.auth-response`. Close with `auth-expired` on timeout or nonce mismatch.

### Phase 2 — ASP.NET Core Integration (`CulpeoStream.AspNetCore`)

- Middleware that upgrades WebSocket connections to CulpeoStream sessions
- `ICulpeoStreamHandler` interface for application code to implement
- Dependency injection extensions (`services.AddCulpeoStream(...)`)
- Routing integration: `app.MapCulpeoStream("/ws", handler)`
- Options pattern for server configuration (supported versions, buffer window limits, auth timeout, idle timeout)

### Phase 3 — Client Library (`CulpeoStream.Client`)

- `CulpeoStreamClient` class wrapping `ClientWebSocket`
- Automatic reconnection with exponential backoff
- Session resumption on reconnect (tracks per-stream offsets internally)
- `SendMediaAsync(streamId, byte[], CancellationToken)`
- Event callbacks / `IAsyncEnumerable` for incoming frames

### Phase 4 — HTTP/2 Transport (`CulpeoStream.Http2`)

Implement an HTTP/2 transport to validate the transport-agnostic design with a second concrete transport. Uses `System.Net.Http.HttpClient` with `HttpVersion.Version20` and `HttpVersionPolicy.RequestVersionExact`.

- New project: `src/CulpeoStream.Http2/`
- `CulpeoHttp2Client` — connects via HTTP/2 POST, bidirectional streaming via request/response bodies
- `CulpeoHttp2Server` — ASP.NET Core minimal API endpoint accepting HTTP/2 streaming connections
- Frame framing per Addendum C: 1-byte type octet + 4-byte length prefix
- `ICulpeoTransport` abstraction shared with the WebSocket transport (if not already present)
- TLS required by default; `AllowHttp2Cleartext` opt-in for development
- Interop test: `CulpeoHttp2Client` ↔ existing `CulpeoStream.AspNetCore` WebSocket server (session layer), and `CulpeoStreamClient` (WS) ↔ `CulpeoHttp2Server`
- All existing tests must pass; add transport-specific and interop tests

### Phase 5 — NativeAOT + Source Generators (`CulpeoStream.SourceGen`)

Make `CulpeoStream.Core` fully trim-safe and NativeAOT-compatible, and introduce a Roslyn source generator that eliminates message-type boilerplate for application developers.

**NativeAOT / Trim Safety:**
- Audit all reflection usage in `CulpeoStream.Core`, `CulpeoStream.AspNetCore`, `CulpeoStream.Client`, and `CulpeoStream.Http2`
- Replace `Type.GetType()`, `Activator.CreateInstance`, and dynamic dispatch with source-generated alternatives or `[DynamicallyAccessedMembers]` annotations
- Add `[RequiresUnreferencedCode]` / `[RequiresDynamicCode]` on any APIs that can't be made trim-safe, with descriptive messages
- Add a `PublishAot` test project (`tests/CulpeoStream.AotTests/`) that publishes with `<PublishAot>true</PublishAot>` and asserts zero ILC warnings
- Target `net9.0` minimum for this package (NativeAOT support is significantly better in .NET 9)

**Roslyn Source Generator (`CulpeoStream.SourceGen`):**
- New project: `src/CulpeoStream.SourceGen/` (a `netstandard2.0` analyzer assembly)
- Attribute `[CulpeoStreamHandler]` on a class implementing `ICulpeoStreamHandler` — generates:
  - `RegisteredStreams` property populated from `[DeclareStream]`-annotated fields
  - `OnMessageAsync` dispatch table (no reflection, no dictionary lookups — switch expression over event names)
  - `HandleMediaAsync` routed by stream ID to strongly-typed method overloads
- Attribute `[DeclareStream(id, type, purpose, contentType)]` on handler fields — generates stream registration boilerplate
- Generates clean, readable, IDE-navigable code (not cryptic templates)
- Source generator must produce code that compiles under NativeAOT with zero warnings
- Tests: `tests/CulpeoStream.SourceGen.Tests/` — use Roslyn `CSharpGeneratorDriver` to test generated output directly

**Definition of done for Phase 5:**
- `dotnet publish -c Release -r linux-x64 --self-contained` with `<PublishAot>true</PublishAot>` on the AOT test project produces a binary with zero ILC trim warnings
- Source generator produces compilable, correct dispatch code for at least: zero streams, one stream, three streams (mixed types), duplicate stream ID (error diagnostic)
- All existing 103+ tests continue to pass on net8.0
- New tests exercise all `[CulpeoStreamHandler]` and `[DeclareStream]` combinations
- DECISIONS.md updated with NativeAOT strategy and source generator design choices

## Technical Requirements

- Target **net8.0** minimum
- Use `System.Net.WebSockets` for the WebSocket transport
- Frame parsing must be allocation-efficient — use `ReadOnlySequence<byte>` / `System.IO.Pipelines` for the header parser
- All public APIs must be `async`/`await` with `CancellationToken` support throughout
- No external dependencies in `CulpeoStream.Core` beyond the BCL
- `CulpeoStream.AspNetCore` may depend on `Microsoft.AspNetCore.*`
- All session IDs and auth nonces must use `RandomNumberGenerator.GetBytes()`

## Concurrency Guidelines

**Prefer atomics over locks.** Only reach for `SemaphoreSlim`, `lock`, or other blocking primitives when the problem genuinely cannot be solved with atomic operations:

- **Use `Interlocked.CompareExchange` / `Interlocked.Exchange`** for state flags, "already started" guards, and reference swaps. These are non-blocking, allocation-free, and compose well with `async` code.
- **Use `SemaphoreSlim(1,1)`** only when you need to serialize a critical section that spans `await` boundaries (e.g. `_sendLock` where frame serialization and send must be atomic across an async call).
- **Never use `lock` around async code** — it cannot be awaited and will deadlock or block threadpool threads.
- Document the reason for each lock/semaphore in a comment. If you can't explain why an atomic wouldn't suffice, use an atomic.

Examples:
```csharp
// ✅ Atomic — fast, non-blocking, correct for "prevent double-connect"
private int _connectState = 0;
if (Interlocked.CompareExchange(ref _connectState, 1, 0) != 0)
    throw new InvalidOperationException("Already connected.");

// ✅ SemaphoreSlim — needed because the critical section spans an await
await _sendLock.WaitAsync(ct); // serialize frame write + network send
try { ... await _ws.SendAsync(...); }
finally { _sendLock.Release(); }
```

## Security Requirements

Work closely with the Security Agent. Specifically:

- Never log or expose bearer tokens in exceptions, traces, or debug output
- Auth nonces must be single-use — store in a `HashSet`, discard after validation
- Session IDs must be at least 128 bits of entropy
- Enforce `wss://` in the client by default; allow `ws://` only via explicit opt-in flag with a compiler warning
- Rate-limit connection attempts per IP if running in server mode (configurable, default 10/min)
- Validate `Content-Type` on every media frame against the declared stream content type

## Interaction with Other Agents

- **Security Agent** will review your decision log and code. Treat their findings as bugs, not suggestions.
- **C++ Core Agent** is building a native core library. Align on shared interop test fixtures in `interop/`.
- **TypeScript Agent** is your primary interoperability counterpart — your server must talk to their client and vice versa. Write interop tests in `interop/`.

## Repository Structure

```
implementations/csharp/
  src/
    CulpeoStream.Core/
    CulpeoStream.AspNetCore/
    CulpeoStream.Client/
  tests/
    CulpeoStream.Core.Tests/
    CulpeoStream.AspNetCore.Tests/
    CulpeoStream.Client.Tests/
  samples/
    EchoServer/
    VoiceClient/
  DECISIONS.md        ← your decision log
```

## Definition of Done

- All MUST requirements from the spec are implemented and covered by tests
- Frame parser round-trips cleanly (parse → serialize → parse produces identical output)
- Session lifecycle state machine is tested for all valid and invalid transitions
- Echo server sample compiles and runs
- DECISIONS.md is current and covers all non-trivial choices made
- Security Agent has reviewed Phase 1 and Phase 2 with no outstanding critical findings
