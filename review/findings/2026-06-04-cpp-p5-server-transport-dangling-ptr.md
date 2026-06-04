# `PyServerTransport` holds dangling `IAsyncTransport*` after session ends (use-after-free + TOCTOU)

**Severity:** High

**Location:** `implementations/cpp/culpeostream-py/src/bindings.cpp` lines 487–559
(`PyServerTransport` class — constructor, `send_text`, `send_binary`, `close_transport`,
`check_valid`, `spawn_void`)

## Description

`PyServerTransport` stores the `IAsyncTransport*` it receives in `handle()` as a raw pointer:

```cpp
class PyServerTransport {
    culpeo::IAsyncTransport* transport_;   // line 544
    asio::io_context*        ioc_;         // line 545
    ...
};
```

`IAsyncTransport` is owned by the H2 server's connection-management layer.  After
`PyH2SessionHandler::handle()` exits (on EOF, network error, or server stop), the H2 server
infrastructure destroys the transport object.  At the same moment, a Python reference to
`PyServerTransport` may still be live in user code — `accept()` returns a `shared_ptr` and
there is no reference back from `PyServerTransport` to `PyH2Server` to prevent this.

### Problem 1 — dangling pointer after transport destroyed

Once `handle()` exits, `set_eof()` sets `valid_ = false` and closes the receive queue.  However:

1. `set_eof()` does **not** null out `transport_`.
2. There is no happens-before guarantee between `set_eof()` completing and the H2 server
   deallocating the transport object.  If the server tears down the transport on the same thread
   tick that `set_eof()` runs, the pointer is stale immediately.
3. A subsequent `send_text` / `send_binary` / `close_transport` call from Python that races past
   `check_valid()` will pass a dangling pointer into `asio::co_spawn`.

### Problem 2 — TOCTOU race in `check_valid()` + use

```cpp
void send_text(py::bytes data) {
    check_valid();                                // ← sees valid_ == true
    auto vec = bytes_to_vec(data);
    spawn_void(transport_->send_text(...));       // ← transport_ may be dangling by here
}
```

`valid_` is set to `false` (with `memory_order_release`) in `set_eof()`, which runs on the
io_thread.  Between `check_valid()` returning and `spawn_void` posting the coroutine, the
session can end and the transport pointer can become stale.  `std::atomic` provides no way to
atomically "check and lock" a raw pointer; the check-then-use window is inherently racy.

### Problem 3 — `ioc_` raw pointer outlives `PyH2Server`

`ioc_` is a raw pointer to `PyH2Server::ioc_`.  If the Python user destroys `PyH2Server`
(running `~PyH2Server()`, which joins the io_thread) while still holding a reference to a
`PyServerTransport`, then:

- `ioc_` is dangling.
- `spawn_void(*ioc_, ...)` at line 556 dereferences the dangling pointer.

There is no ownership link from `PyServerTransport` to `PyH2Server`.

## Impact

- Calling `send_text`, `send_binary`, or `close_transport` on a `ServerTransport` object after
  the session closes (even after `is_valid` returns `False`, due to the TOCTOU race) can corrupt
  heap memory, crash with SIGSEGV, or silently produce wrong behavior.
- Destroying `H2Server` while a `ServerTransport` is still referenced also leads to a crash.
- Both scenarios are easy to trigger in production code that uses session callbacks and keeps
  transport references alive across reconnects.

## Suggested Fix

Replace the raw pointer with a lifetime-safe mechanism:

1. **`std::weak_ptr` into a shared wrapper**: Wrap `IAsyncTransport` in a
   `shared_ptr<AsyncTransportHolder>` inside `handle()`.  Pass `weak_ptr` to
   `PyServerTransport`.  In `send_text` etc., lock the `weak_ptr`; if it returns `nullptr`,
   throw `RuntimeError("session closed")` — atomically combining the validity check and the
   use.

2. **For `ioc_`**: Pass a `std::shared_ptr<asio::io_context>` (or a
   `std::shared_ptr<PyH2Server>`) into `PyServerTransport` so that the io_context is kept alive
   for at least as long as any `PyServerTransport` derived from it.

3. **Remove `check_valid()` / TOCTOU pattern**: Once `weak_ptr::lock()` is used, the atomic
   check-then-use becomes a single operation.
