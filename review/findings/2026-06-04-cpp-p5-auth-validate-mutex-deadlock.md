# `on_auth_validate` invoked with session mutex held — re-entrant session call deadlocks

**Severity:** Medium

**Location:** `implementations/cpp/libculpeo-session/src/session.cpp` line 479;
`implementations/cpp/culpeostream-py/src/bindings.cpp` lines 252–256 (lambda);
`implementations/cpp/libculpeo-session/include/culpeo/session.hpp` line 7 (misleading doc comment)

## Description

The session.hpp header states at line 7–10:

> Thread safety: All public methods are safe to call concurrently from separate threads.
> The session holds a single std::mutex that is acquired for the duration of each call,
> **released before any transport I/O. Callbacks are invoked WITHOUT the mutex held.**

However, `on_auth_validate` is invoked at `session.cpp` line 479 **while the mutex is still
locked** — there is no `lock.unlock()` call in the normal (non-error) path of `handle_init`
before reaching line 479.

The Python binding wraps `on_auth_validate` in a lambda that acquires the GIL and calls into
Python.  A natural thing for user code to do inside this callback is to inspect or update the
session — for example, looking up a session ID to cross-validate the token, or calling
`session.close()` on validation failure:

```python
def validate(token):
    if token != expected:
        session.close("unauthorized", "bad token")   # ← tries to acquire session mutex
        return False
    return True
```

Because `std::mutex` is not re-entrant, the callback calling back into any `Session` method
(`state()`, `close()`, `send_ping()`, etc.) from the same thread that holds the mutex will
deadlock immediately.  The GIL and the session mutex are then both held in conflicting orders
on the same thread, producing a hard deadlock with no timeout.

Note: `on_auth_response` (session.cpp line 855) is also invoked after `lock.unlock()`, so it
does NOT share this problem — the lock is explicitly released before the callback there.  The
inconsistency itself is a latent bug for future maintenance.

## Impact

- Hard deadlock (infinite block) when the Python `on_auth_validate` callback calls any
  `Session` method on the same session object.
- The bindings docstring (line 757–787) explicitly says "Thread-safe: all methods may be
  called from concurrent Python threads", with no caveat about the mutex being held during
  validation callbacks.  Users will write re-entrant callbacks and hit the deadlock.
- The session.hpp comment promising "Callbacks are invoked WITHOUT the mutex held" is
  incorrect for this callback, which is a maintenance hazard.

## Suggested Fix

In `session.cpp`, release the mutex before invoking `on_auth_validate` (mirroring the pattern
used for `on_auth_response` at line 853):

```cpp
// In handle_init, before line 479:
auto auth_cb = callbacks.on_auth_validate;
std::string bearer_copy(bearer);   // copy before unlock
lock.unlock();
if (auth_cb) auth_ok = auth_cb(bearer_copy);
// Do NOT use `bearer` (string_view into frame) after unlock if frame buffer may move.
```

Also update the bindings docstring for `Session` to document that `on_auth_validate` is (or
will be) called without the session mutex, and warn that calling back into the session from
within `on_auth_validate` is still not supported (to avoid confusion during the transition).
