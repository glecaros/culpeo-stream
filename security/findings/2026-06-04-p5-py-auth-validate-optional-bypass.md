## Finding: Python bindings — on_auth_validate=None silently removes authentication

**Severity:** Medium
**Target:** C++
**Phase:** Phase 5 — Python bindings

### Description

`bindings.cpp` lines 251–257 register the `on_auth_validate` callback
conditionally:

```cpp
// on_auth_validate — required for auth enforcement
if (!on_auth_validate.is_none()) {
    cbs.on_auth_validate = [cb = std::move(on_auth_validate)](
                               std::string_view token) -> bool {
        py::gil_scoped_acquire acquire;
        return cb(std::string(token)).cast<bool>();
    };
}
```

If a Python developer creates a `Session` **without** providing
`on_auth_validate` (or explicitly passes `None`), `cbs.on_auth_validate` is
never populated.  The `Session` class signature makes this parameter optional:

```python
session = culpeostream.Session(transport=transport)
```

The comment says "required for auth enforcement", but there is no runtime
assertion, no `ValueError`, and no compile-time enforcement.  The behaviour when
the C++ `culpeo::session::Session` receives a `culpeo.init` frame without an
auth callback is not defined in the binding documentation.  Depending on the
underlying C++ session implementation, the two most likely outcomes are:

(a) **All connections accepted** — the session treats a missing callback as
    "no auth required" and transitions to `established` unconditionally.  This
    is an authentication bypass.

(b) All connections rejected — the session treats a missing callback as
    "auth unavailable" and always sends `culpeo.init-error(unauthorized)`.

Outcome (a) is the more common default in callback-based C++ APIs and is the
dangerous case.  The test suite does **not** test what happens when a
`culpeo.init` frame is sent to a session created without `on_auth_validate`,
leaving this behaviour unspecified from a security standpoint.

### Attack Scenario

1. Developer writes a Python server integration:
   ```python
   session = culpeostream.Session(transport=ws_transport)  # forgot auth
   session.process_control_frame(client_bytes)
   ```
2. If the C++ default is "allow all", every `culpeo.init` frame — regardless
   of the `Authorization` header — succeeds.
3. Any client that knows the server URL can establish a session with no token.

### Impact

If outcome (a) applies: complete authentication bypass for all sessions created
without an explicit `on_auth_validate` callback.  Unauthenticated callers gain
full session privileges.

### Suggested Fix

1. **Enforce at the binding layer**: raise `ValueError` at construction time if
   `on_auth_validate` is `None`:

   ```cpp
   if (on_auth_validate.is_none()) {
       throw std::invalid_argument(
           "on_auth_validate is required; pass a callable that verifies bearer tokens");
   }
   ```

2. If a "no-auth" mode is intentionally desired (e.g., for testing), provide an
   explicit opt-in:
   ```python
   culpeostream.Session(transport=t, on_auth_validate=culpeostream.ALLOW_ALL_AUTH)
   ```
   so the developer is forced to make the decision consciously.

3. Add a test:
   ```python
   def test_missing_auth_validate_raises():
       with pytest.raises((TypeError, ValueError)):
           culpeostream.Session(transport=transport)  # no on_auth_validate
   ```

4. Clarify the C++ `SessionCallbacks` documentation: state explicitly whether a
   missing `on_auth_validate` defaults to allow-all or deny-all.

### Spec Reference

CulpeoStream spec §4.1 (Authentication); spec requires every session to be
authenticated via the `Authorization` header in `culpeo.init`.
