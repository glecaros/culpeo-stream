## Finding: TypeScript frame.event switch lacks compile-time exhaustiveness check

**Severity:** Low
**Target:** TypeScript
**Phase:** Phase 1

### Description

The `CulpeoClientSession.receive` and `CulpeoServerSession.receive` methods both dispatch on `frame.event` in a `switch` statement. The `default` branch forwards unknown events to `handleApplication`, which is the correct runtime behavior for application-defined events. However, neither switch uses a TypeScript `never`-based exhaustiveness pattern or an `assertNever` helper to guarantee at compile time that all known protocol events are explicitly handled.

Current pattern (both client and server `receive`):
```typescript
switch (frame.event) {
    case "culpeo.ping":   ...
    case "culpeo.pong":   ...
    case "culpeo.auth-refresh": ...
    case "culpeo.close":  ...
    case "culpeo.init":
    case "culpeo.init-ack":
    case "culpeo.init-error":
    case "culpeo.auth-response":
        await this.failWithClose("protocol-error", "...invalid in current state...");
        return;
    default:
        this.handleApplication(frame as ApplicationEventFrame);
}
```

If a new protocol event is added (e.g., `culpeo.resume-ack`) and is added to the `KnownProtocolEvent` union type but not explicitly handled in one of these switch statements, TypeScript will not produce a compile error. The new event silently falls into `handleApplication`, which dispatches it as an application event—a protocol-semantic error that is invisible to the type checker.

### Attack Scenario

1. A developer adds a new spec event `culpeo.session-transfer` to the `KnownProtocolEvent` union type.
2. They add handling in `CulpeoServerSession.receive` but forget to add the corresponding case in `CulpeoClientSession.receive`.
3. TypeScript compiles cleanly.
4. A server sends `culpeo.session-transfer` to a client; the client routes it to `handleApplication` and emits a `session-notification` event rather than the intended state transition.
5. In a security context, a malicious server could send a frame with a protocol-reserved event name to confuse a poorly-guarded client state machine.

### Impact

Low in isolation; the current event set is handled correctly. The risk escalates with each future protocol event addition. Missing exhaustiveness checks are a well-known source of protocol implementation bugs.

### Proposed Mitigation

Add an `assertNever` utility:

```typescript
function assertNever(x: never, message: string): never {
    throw new CulpeoError("protocol-error", message);
}
```

Refactor the default branch in `receive` to target only the `KnownProtocolEvent` type for the "invalid in current state" arm, and add an explicit exhaustiveness check on the known-events enum. The `default` branch should remain for non-protocol (application-defined) events, but a guard type-narrowing step before reaching it would catch unhandled known protocol events.

Alternative: define a discriminated union of known control frame types and use TypeScript's type narrowing — when the compiler narrows to `never` in a `default` branch, it guarantees all union arms are covered.

### Spec Reference

TypeScript implementation checklist: "Discriminated union exhaustiveness checks present for all protocol event handling"
