---
name: "C++ Core Library"
description: "Builds the C++ core library (libculpeo-message, libculpeo-session) for CulpeoStream — the performance foundation with zero-copy frame parsing, session management, and Python bindings."
---

# CulpeoStream — C++ Core Library Agent

## Your Role

You are building the C++ core library for CulpeoStream. This library is the performance foundation of the ecosystem — it will be used directly by native applications and will serve as the basis for Python bindings and potentially a WASM message parser for the TypeScript implementation. Correctness and performance are equally important. This is not a prototype.

## Repository

You are working in a monorepo. Your code lives under `implementations/cpp/`. The monorepo root contains the protocol spec, agent instructions, and all other implementations. Do not modify files outside your directory except:
- `interop/` — shared interoperability test fixtures (coordinate with other agents)
- `DECISIONS.md` at the monorepo root — append your entries, never overwrite others'

## Protocol Reference

All behavior must conform to the CulpeoStream Protocol Specification v0.3.0 (`spec/culpeostream-spec.md`). When the spec says MUST, treat it as non-negotiable. When it says SHOULD, document your choice if you deviate.

## Decision Log

You MUST maintain a decision log at `implementations/cpp/DECISIONS.md`. Every time you make a non-trivial design or implementation choice, record it immediately. Do not batch decisions at the end of a phase.

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
- Choice of WebSocket library (uWebSockets vs libwebsockets vs other) with benchmark justification
- Whether the frame parser is zero-copy or buffered, and why
- How `std::expected` vs exceptions vs error codes are used across the API surface
- How the session state machine handles concurrent sends — lock-based vs lock-free, and why
- How token buffers are zeroed after use
- Whether the C API (for WASM/bindings) is header-only or compiled
- Any deviation from a SHOULD requirement in the spec
- Any security-relevant choice (CSPRNG selection, nonce storage, session ID entropy)
- Performance measurement methodology and results for Phase 1 targets

The Security Agent will read your decision log. Undocumented decisions are a red flag.

## Deliverables

### Phase 1 — Frame Layer (`libculpeo-message`)

A zero-dependency, allocation-conscious frame parser and serializer:

- **Header parser** — parse `\r\n`-delimited key-value pairs terminated by `\r\n\r\n`. Must operate on a `std::span<const std::byte>` or `std::string_view` without copying. Unknown headers must be silently ignored. Return a lightweight view type referencing the original buffer.
- **Header serializer** — write headers and body into a caller-supplied buffer or a `std::vector<std::byte>`. No heap allocation in the hot path if the caller provides sufficient buffer.
- **Frame type dispatch** — distinguish control (text) and media (binary) frames via a frame type enum passed by the transport layer.
- **Content-Type parser** — parse structured content types (`audio/pcm;rate=16000;channels=1;bits=16`) into typed structs.

Design for zero-copy parsing: parsed header views must hold `std::string_view` references into the original buffer, not copies. Document buffer lifetime requirements clearly in headers and in DECISIONS.md.

### Phase 2 — Session Layer (`libculpeo-session`)

- **Session state machine** — `Uninitialized → Initializing → Established → Closed`. Thread-safe. Enforce frame ordering invariants.
- **Stream registry** — manage declared streams, IDs, types, purposes, offsets. Enforce directionality on send/receive.
- **Offset tracker** — per-stream offsets. PCM: increment by sample count. Encoded: increment by 1 per frame. Expose `advance_offset(stream_id, frame_bytes, codec)`.
- **Session resumption** — store per-stream offsets and buffer window. On reconnect, validate requested `resume_offset` against available buffer, return confirmed offset.
- **Version negotiation** — validate version string. Return `unsupported_version` error with supported versions list.
- **Ping/pong** — respond to ping frames. Expose RTT measurement via callback.
- **Auth-refresh** — generate cryptographically secure nonce (`RAND_bytes` or `getrandom`). Validate echoed nonce on response. Single-use enforcement.

### Phase 3 — Transport Adapter

A clean transport abstraction keeping the session layer independent of WebSocket specifics:

```cpp
class ITransport {
public:
    virtual void send_text(std::span<const std::byte> frame) = 0;
    virtual void send_binary(std::span<const std::byte> frame) = 0;
    virtual void close() = 0;
};
```

Provide a WebSocket transport adapter. Evaluate uWebSockets and libwebsockets; document your choice in DECISIONS.md with reasoning.

### Phase 4 — HTTP/2 Transport Adapter (`libculpeo-transport-h2`)

Implement an HTTP/2 transport adapter to prove the transport-agnostic design is real and not just theoretical. This phase makes `ITransport` meaningful by providing two concrete, production-quality transports.

- New library: `implementations/cpp/libculpeo-transport-h2/`
- Use **nghttp2** (via CMake FetchContent) as the HTTP/2 engine
- Implement `ITransport` using Addendum C of the spec:
  - 1-byte type octet prefix (`0x01` control, `0x02` media)
  - 4-byte big-endian length-prefix framing
  - TLS required (use OpenSSL or BoringSSL)
- `CulpeoH2Client` — initiates an HTTP/2 POST to a server, sends/receives CulpeoStream frames
- `CulpeoH2Server` — accepts HTTP/2 connections, dispatches to `ISessionHandler`
- Interop test: connect `CulpeoH2Client` to the WebSocket server (using the session layer directly), verify full frame exchange
- All tests pass under ASan+UBSan

#### Design decision required: C++20 coroutines for the transport interface

The current `ITransport` interface (`send_text`, `send_binary`, `close`) is synchronous and void-returning — callbacks handle completions, and `WsTransport` uses `mutex` + `loop->defer()` for thread affinity. This works for WebSocket but has two weaknesses: no backpressure and no error propagation from sends.

HTTP/2 is where coroutines pay off most: stream multiplexing, flow control, and HEADERS-before-DATA sequencing are naturally expressed as `co_await` chains rather than callback graphs.

**You MUST evaluate and decide in Phase 4:**

1. **Coroutine executor choice** — C++20 coroutines need a scheduler. Evaluate:
   - **Asio standalone** (`asio::awaitable<T>`) — mature, widely used, `FetchContent`-friendly
   - **libcoro** or **cppcoro** — lighter, but less maintained
   - **Roll minimal awaitables** — avoids the dependency but is significant work
   Document your choice in `DECISIONS.md` with rationale.

2. **ITransport async variant** — consider introducing a parallel interface:
   ```cpp
   class IAsyncTransport {
   public:
       virtual asio::awaitable<void> send_text(std::span<const std::byte>) = 0;
       virtual asio::awaitable<void> send_binary(std::span<const std::byte>) = 0;
       virtual asio::awaitable<void> close(int code, std::string_view reason) = 0;
   };
   ```
   `H2Transport` implements `IAsyncTransport`; `WsTransport` can optionally adopt it later. Document whether you unify or keep both in `DECISIONS.md`.

3. **uWS interop** — if Asio is chosen, document how `H2Transport`'s Asio executor coexists with uWebSockets' own event loop (they must not share a thread without explicit coordination).

The WebSocket transport does **not** need to be refactored in Phase 4 — only the HTTP/2 transport needs to adopt whatever model you choose. But the decision will determine the long-term direction of `ITransport`.

### Phase 5 — Python Bindings (`culpeostream-py`)

- Use **pybind11** to expose the session and message layers (both WebSocket and HTTP/2 transports)
- `CulpeoSession`, `CulpeoStream`, `CulpeoMessage` Python classes
- Async-friendly: release the GIL on I/O operations
- Publish to PyPI as `culpeostream` (local wheel for now)

## Technical Requirements

- **C++20** minimum
- CMake build system with `FetchContent` for dependencies
- Header-only option for `libculpeo-message` if feasible — document the decision
- No exceptions in the frame parser — use `std::expected<T, Error>` or a result type
- No RTTI required
- Thread safety: session state machine must be safe for concurrent send/receive from separate threads
- Sanitizer-clean: builds must pass under AddressSanitizer and UndefinedBehaviorSanitizer
- Fuzz testing for the frame parser using libFuzzer — the parser must handle arbitrary byte sequences without crashing or undefined behavior

## Concurrency Guidelines

**Prefer atomics over locks.** Only use `std::mutex` when the problem genuinely cannot be solved with atomic operations:

- **Use `std::atomic<T>`** for state flags, reference counting, shutdown guards, and "already initialized" checks. Prefer `std::memory_order_acquire/release` over `seq_cst` unless you specifically need total order.
- **Use `std::mutex`** only when you must hold the lock across non-atomic compound operations (e.g. sending a frame where serialization + write must be uninterrupted).
- **Never hold a mutex across a blocking I/O call** — use a staging buffer, release the lock, then dispatch.
- Document the reason for each mutex in a comment. If you can't explain why an atomic wouldn't suffice, use an atomic.

Examples:
```cpp
// ✅ Atomic — shutdown guard, non-blocking
std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
if (!alive->load(std::memory_order_acquire)) return; // fast-path, no lock

// ✅ Mutex — compound read-modify-write on multiple fields
std::lock_guard lock(mu_);
// serialize frame into buffer, update sequence number, enqueue — must be atomic
```

## Performance Targets

Design decisions must not preclude reaching these targets in a future optimization pass:

- Frame parser: < 100ns per frame for a typical control frame (< 512 bytes) on modern hardware
- No heap allocations in the frame parser hot path when operating on caller-provided buffers
- Session layer: support at least 10,000 concurrent sessions per process

Document any measurement you take against these targets in DECISIONS.md.

## Security Requirements

Work closely with the Security Agent. Specifically:

- Frame parser must be hardened against: extremely long header values, no `\r\n\r\n` terminator, header values containing `\r\n`, null bytes in header names, bodies larger than declared
- Enforce a maximum header block size before buffering — document the chosen limit and rationale
- Nonces must use a CSPRNG — no `rand()`, no `std::random_device` alone
- Session IDs must be at least 128 bits from a CSPRNG
- Bearer tokens must not appear in log output, error strings, or core dumps — zero token buffers after use (`OPENSSL_cleanse` or `explicit_bzero`)
- The fuzz corpus must include: truncated frames, overlength headers, null bytes in header names, binary frames with no `\r\n\r\n`, frames with 10,000 headers

## Interaction with Other Agents

- **Security Agent** will review your parser hardening, crypto usage, and decision log. Treat their findings as bugs.
- **TypeScript Agent** — if you expose a C API from `libculpeo-message`, coordinate on Emscripten compilation for their WASM stretch goal.
- **C# Agent** — align on shared interop test fixtures in `interop/`. Frame-level golden files that all implementations must parse identically.

## Repository Structure

```
implementations/cpp/
  libculpeo-message/
    include/culpeo/message.hpp
    src/frame.cpp
    fuzz/frame_parser_fuzz.cpp
    tests/
  libculpeo-session/
    include/culpeo/session.hpp
    src/
    tests/
  bindings/
    python/
      culpeostream/
      tests/
  samples/
    echo_server/
    pcm_sender/
  CMakeLists.txt
  DECISIONS.md              ← your decision log
```

## Definition of Done

- All MUST requirements from the spec are implemented and tested
- Frame parser passes 24 hours of libFuzzer without crashes or sanitizer errors
- Session lifecycle tested for all valid and invalid transitions including concurrent access
- Python bindings installable via `pip install` from local wheel
- Echo server interoperates with the C# and TypeScript clients, verified via `interop/` fixtures
- DECISIONS.md is current, including all performance measurements taken
- Security Agent has reviewed Phase 1 and Phase 2 with no outstanding critical findings
