# Python callbacks can throw through `noexcept` Session methods → `std::terminate`

**Severity:** High

**Location:** `implementations/cpp/culpeostream-py/src/bindings.cpp` lines 252–296 (callback lambdas);
`implementations/cpp/libculpeo-session/include/culpeo/session.hpp` lines 237–259 (noexcept declarations);
`implementations/cpp/libculpeo-session/src/session.cpp` lines 479, 785, 855, 896, 1134 (callback invocation sites)

## Description

Every Python callback registered via `PySession` is wrapped in a C++ lambda that calls
`py::gil_scoped_acquire` and then invokes the Python callable.  Both the `py::error_already_set`
path (Python callable raises an exception) and the `py::cast_error` path (`cast<bool>()` fails
on a non-bool return value) can throw a live C++ exception out of the lambda.

These lambdas are stored as `std::function<…>` members of `SessionCallbacks` and are invoked
directly from within:

- `Session::process_control_frame()` — declared **`noexcept`** (session.hpp line 237)
- `Session::process_media_frame()` — declared **`noexcept`** (session.hpp line 241)
- `Session::send_ping()` / pong handler — `noexcept`
- `Session::close()` — `noexcept`

The C++ standard mandates that any exception escaping a `noexcept` function causes an immediate
call to `std::terminate()` — the process crashes, with no chance to handle the error or
propagate it back to Python.

Concrete crash path for `on_auth_validate`:

```
Python thread                    io/session thread
─────────────────────────────────────────────────────────
session.process_control_frame(raw_bytes)
  → py::gil_scoped_release
  → session_->process_control_frame(parsed)    [noexcept]
      → handle_init(f, lock)                   [noexcept]
          → callbacks.on_auth_validate(bearer)
              → py::gil_scoped_acquire
              → cb(std::string(token))  ← Python raises ValueError
              → py::error_already_set thrown
          ← exception propagates through noexcept boundary
          → std::terminate() — CRASH
```

The same applies to `on_auth_response`, `on_rtt`, `on_close`, and `on_media_received`.

## Impact

Any Python exception raised inside a registered callback (validation failure, attribute error,
unexpected return type, etc.) crashes the entire process immediately.  This is a silent crash
from the Python caller's perspective — the exception is never surfaced to Python; the interpreter
simply dies.

## Suggested Fix

Wrap every callback lambda in a `try/catch` that intercepts C++ exceptions before they escape
into `noexcept` territory.  For callbacks returning `bool`, store the exception and return a
safe default; after the `noexcept` function returns, re-raise.  Example pattern for
`on_auth_validate`:

```cpp
// Shared exception holder injected into all lambdas.
auto exc_holder = std::make_shared<std::exception_ptr>(nullptr);

cbs.on_auth_validate = [cb = std::move(on_auth_validate),
                        exc = exc_holder](std::string_view token) -> bool {
    py::gil_scoped_acquire acquire;
    try {
        return cb(std::string(token)).cast<bool>();
    } catch (...) {
        *exc = std::current_exception();
        return false;  // safe default: reject auth
    }
};
```

Then in `PySession::process_control_frame`, after the `noexcept` C++ call returns, check
`exc_holder` and call `std::rethrow_exception` before re-acquiring the GIL for the
pybind11 boundary.

For void callbacks (`on_media_received`, `on_close`, `on_rtt`) the same pattern applies with
an empty catch body that stores the exception for deferred re-raise.
