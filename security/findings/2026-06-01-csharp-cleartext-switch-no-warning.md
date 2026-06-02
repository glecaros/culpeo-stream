# [SEC-031] C# `AllowHttp2Cleartext` sets a process-wide, irreversible switch with no runtime warning

**Severity:** Medium  
**Component:** C#  
**Phase:** 4  
**Status:** Open  

## Description

`CulpeoHttp2Client` enables cleartext HTTP/2 by setting a process-wide AppContext switch:

```csharp
if (options.AllowHttp2Cleartext)
{
    AppContext.SetSwitch(
        "System.Net.Http.SocketsHttpHandler.Http2UnencryptedSupport",
        true);
}
```

Two properties of this switch create security risk:

1. **Process-wide and irreversible:** Once set, `Http2UnencryptedSupport` cannot
   be unset within the same process.  Any subsequent `CulpeoHttp2Client`
   instance — or any unrelated `SocketsHttpHandler` in the same process — will
   also be able to make cleartext HTTP/2 connections, even if those callers did
   not intend to allow it.

2. **No runtime warning:** The `AllowHttp2Cleartext` path emits no log message,
   no `Trace`, and no `Debug` output.  An operator who accidentally enables the
   flag in production has no indication in any log file that cleartext HTTP/2 is
   active.

By contrast:
- The TypeScript server emits `console.warn` when `allowInsecure: true`.
- The C++ client/server use a distinct `AllowCleartext{}` tag type (also
  without a warning, but documented in DECISIONS.md as an accepted gap).

The C# DECISIONS.md notes: *"No compiler warning is emitted … because .NET does
not provide a `[Obsolete]`-based warning path for runtime switches; the option
documentation serves as the warning."*  Documentation-only warnings are
insufficient for a security-sensitive setting.

## Impact

If a production service accidentally passes `AllowHttp2Cleartext = true` (e.g.,
via misconfiguration, a feature flag, or a test configuration leaking into
production), all HTTP/2 connections in the process — including those to
unrelated internal services — become permitted to run over cleartext TCP.
Bearer tokens exchanged with any CulpeoStream server, as well as any other
HTTP/2 traffic in the process, are exposed to passive network observation.

The risk is amplified in microservice environments where many services share
infrastructure and a single `.env` misconfiguration affects the whole pod.

## Location

`implementations/csharp/src/CulpeoStream.Http2/CulpeoHttp2Client.cs`,
`CulpeoHttp2Client` constructor, the `AllowHttp2Cleartext` branch.

## Recommendation

Emit a structured log or `Trace.WriteLine` warning when cleartext is enabled,
so it always appears in production log aggregators:

```csharp
if (options.AllowHttp2Cleartext)
{
    AppContext.SetSwitch(
        "System.Net.Http.SocketsHttpHandler.Http2UnencryptedSupport",
        true);

    // Emit to every ILogger-connected sink and to Trace listeners so that
    // monitoring systems can alert on this setting being active.
    System.Diagnostics.Trace.TraceWarning(
        "[CulpeoStream] AllowHttp2Cleartext is enabled. " +
        "HTTP/2 connections will use cleartext (h2c). " +
        "This MUST NOT be used in production.");
}
```

Alternatively, add an `[Obsolete("For development use only. Not for production.", false)]`
attribute to `AllowHttp2Cleartext` in the options class to surface a
compiler-level warning for any code that sets it.
