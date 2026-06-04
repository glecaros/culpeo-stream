# Generated OnMessageAsync dispatches protocol events that ICulpeoStreamHandler.OnEventAsync never receives

**Severity:** Medium  
**Location:** `src/CulpeoStream.SourceGen/Generators/CodeGenerator.cs` lines 12–22, 101–107, 137–144

## Description

`CodeGenerator.Generate` emits a `ProtocolEvents` array and generates both a dispatch switch and protected virtual stubs for all eight protocol events:

```csharp
private static readonly string[] ProtocolEvents =
[
    "culpeo.init",
    "culpeo.init-ack",
    "culpeo.init-error",
    "culpeo.ping",
    "culpeo.pong",
    "culpeo.auth-refresh",
    "culpeo.auth-response",
    "culpeo.close",
];
```

The generated `OnMessageAsync` is documented as:
```
/// Call this from <see cref="ICulpeoStreamHandler.OnEventAsync"/>.
```

However, `ICulpeoStreamHandler.OnEventAsync` is called only for **application events** — those whose names do not start with `culpeo.`. Protocol events (`culpeo.*`) are handled transparently by `WebSocketTransportAdapter` and never forwarded to `OnEventAsync`. Consequently, if a user calls `OnMessageAsync` from `OnEventAsync` as documented, the `culpeo.*` arms of the switch are permanently unreachable, and the eight generated virtual stubs (`OnCulpeoInitAsync`, `OnCulpeoPingAsync`, etc.) can never be invoked through the normal dispatch path.

There is a secondary correctness problem: the generated `OnMessageAsync` accepts `culpeo.pong` and `culpeo.init-ack` as dispatchable events. These are server-to-client frames in the protocol and should never arrive as inbound events on a server-side handler — including them in the dispatch table is misleading and could cause confusion if a user tries to intercept them.

## Impact

- Developers who override `OnCulpeoPingAsync` expecting to be notified of pings will find it never called.
- The dead stubs add noise to the generated class and may cause confusion during debugging.
- If a future version of the middleware does forward some protocol events to `OnEventAsync`, the existing stubs would silently no-op instead of failing loudly, masking regressions.

## Suggested Fix

Remove `culpeo.*` protocol events from `ProtocolEvents` and from `OnMessageAsync` dispatch. The generated dispatch should cover only application-namespace events, with an `_` catch-all for unknown events — matching the actual contract of `OnEventAsync`.

If there is a genuine use case for hooking protocol events (e.g. for testing or observability), introduce a separate `OnProtocolEventAsync` virtual or a dedicated `ILifecycleObserver` hook, and document clearly that it is not called from `OnEventAsync`.
