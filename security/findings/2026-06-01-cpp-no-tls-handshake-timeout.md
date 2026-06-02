# [SEC-029] C++ H2 server: no TLS handshake timeout — slow TCP connect holds coroutine indefinitely

**Severity:** Medium  
**Component:** C++  
**Phase:** 4  
**Status:** Open  

## Description

In `CulpeoH2Server::run()`, the TLS branch awaits the handshake with no timeout:

```cpp
asio::co_spawn(ioc,
    [ssl_sock, handler_ptr, &ioc]() -> asio::awaitable<void> {
        try {
            co_await ssl_sock->async_handshake(
                asio::ssl::stream_base::server, asio::use_awaitable);
        } catch (...) {
            co_return; // TLS handshake failed; drop connection
        }
        // … session setup …
    },
    asio::detached);
```

An attacker who completes the TCP three-way handshake but then stalls the TLS
handshake (e.g., by sending only the ClientHello and never the subsequent
certificate or Finished message) will leave this coroutine suspended on
`async_handshake` indefinitely.

Because `co_spawn(… asio::detached)` detaches the coroutine, the only resource
bound is the memory for the suspended coroutine frame and the `SslSocket`
object.  There is no watchdog, no idle timer, and no connection count limit to
cap how many such stalled handshakes accumulate.

The same issue applies to the cleartext branch in the degenerate case where a
client connects but never sends the HTTP/2 client preface.

## Impact

An attacker with the ability to open TCP connections can accumulate thousands
of stalled TLS handshakes at negligible cost (one SYN per connection, no data
needed).  Each stalled handshake holds a file descriptor, a coroutine frame,
and the `ssl::stream` object.  At scale this exhausts file descriptors (default
ulimit ~1024–65536) and heap memory, preventing legitimate clients from
connecting.

## Location

`implementations/cpp/libculpeo-transport-h2/src/h2_server.cpp`,
`CulpeoH2Server::run()` — TLS branch `co_spawn` lambda (approximately line 120).

## Recommendation

Wrap the handshake with an Asio timeout using `asio::cancel_after` (Asio 1.28+)
or an explicit `asio::steady_timer`:

```cpp
// Option A — cancel_after (Asio 1.28+)
co_await asio::cancel_after(
    std::chrono::seconds(10),
    ssl_sock->async_handshake(
        asio::ssl::stream_base::server, asio::use_awaitable));

// Option B — explicit timer + awaitable_operators
asio::steady_timer timer(ioc, std::chrono::seconds(10));
auto [order, ec1, ec2] = co_await (
    ssl_sock->async_handshake(asio::ssl::stream_base::server,
                              asio::as_tuple(asio::use_awaitable))
    || timer.async_wait(asio::as_tuple(asio::use_awaitable)));
if (order == 1) co_return; // timer fired first → drop
```

A 10-second handshake timeout is a reasonable production default; expose it as
a `CulpeoH2Server` configuration parameter.  Similarly, add a limit on the
maximum number of concurrent in-flight handshakes (e.g., reject the TCP
connection at `async_accept` if the in-flight count exceeds a threshold).
