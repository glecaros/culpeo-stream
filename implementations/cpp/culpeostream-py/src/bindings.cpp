// CulpeoStream Python Bindings — Phase 5
//
// Exposes the C++ session + message + transport layers to Python via pybind11.
//
// GIL policy (documented in DECISIONS.md):
//   • All methods that perform I/O (send_text, send_binary, process_*_frame,
//     send_media, connect, receive_frame, close_session) release the GIL via
//     `py::gil_scoped_release` so they do not block the Python event loop or
//     other threads during C++ execution.
//   • Callbacks from C++ into Python (on_auth_validate, on_media_received,
//     on_close, on_rtt, on_auth_response, WsTransport send/close) re-acquire
//     the GIL with `py::gil_scoped_acquire` before touching any Python objects.
//
// Zero-copy deviation (documented in DECISIONS.md):
//   • CulpeoMessage copies header values and body into std::string / dict,
//     because Python's memory model does not support C++ buffer views.
//   • send_media / send_text copy the Python bytes object before releasing
//     the GIL, since Python buffers must not be accessed without the GIL.

#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "culpeo/async_transport.hpp"
#include "culpeo/h2_client.hpp"
#include "culpeo/h2_server.hpp"
#include "culpeo/message.hpp"
#include "culpeo/session.hpp"
#include "culpeo/transport_ws.hpp"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/ssl/context.hpp>
#include <asio/use_future.hpp>

#include <atomic>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace py = pybind11;

// ─── Helpers ─────────────────────────────────────────────────────────────────

// Convert py::bytes to std::vector<std::byte> while holding the GIL.
// Call BEFORE releasing the GIL; the returned vector is GIL-independent.
static std::vector<std::byte> bytes_to_vec(py::bytes data) {
    char* buf   = nullptr;
    Py_ssize_t sz = 0;
    if (PyBytes_AsStringAndSize(data.ptr(), &buf, &sz) < 0) {
        throw py::error_already_set();
    }
    return std::vector<std::byte>(
        reinterpret_cast<const std::byte*>(buf),
        reinterpret_cast<const std::byte*>(buf) + static_cast<std::size_t>(sz));
}

// Convert py::bytes to std::string_view — valid only while GIL is held and
// the Python bytes object is alive.
static std::string_view bytes_sv(py::bytes data) {
    char* buf   = nullptr;
    Py_ssize_t sz = 0;
    if (PyBytes_AsStringAndSize(data.ptr(), &buf, &sz) < 0) {
        throw py::error_already_set();
    }
    return {buf, static_cast<std::size_t>(sz)};
}

// Translate culpeo::session::Error to a Python RuntimeError.
[[noreturn]] static void throw_session_error(culpeo::session::Error e) {
    throw std::runtime_error(std::string(culpeo::session::error_message(e)));
}

// ─── Thread-safe queue ────────────────────────────────────────────────────────

template <typename T>
class SafeQueue {
public:
    void push(T item) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            q_.push(std::move(item));
        }
        cv_.notify_one();
    }

    // Returns std::nullopt when the queue is closed and empty.
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return !q_.empty() || closed_; });
        if (q_.empty()) return std::nullopt;
        T v = std::move(q_.front());
        q_.pop();
        return v;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            closed_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex              mu_;
    std::condition_variable cv_;
    std::queue<T>           q_;
    bool                    closed_{false};
};

// ─── CulpeoMessage ────────────────────────────────────────────────────────────
//
// Owning Python-friendly wrapper around ParsedHeadersView. Copies all header
// values into std::string because Python cannot hold C++ string_view lifetime
// guarantees across GC cycles.

class PyCulpeoMessage {
public:
    /// Parse a raw frame from Python bytes.
    /// @param data        Raw frame bytes (header block + CRLF CRLF + body).
    /// @param frame_type  culpeo.FrameType.control (default) or .media.
    /// @param limits      ParseLimits struct (optional).
    PyCulpeoMessage(py::bytes data,
                    culpeo::message::FrameType frame_type,
                    culpeo::message::ParseLimits limits) {
        // Parse while holding the GIL (we read from the Python bytes buffer).
        std::string_view sv = bytes_sv(data);
        auto result = culpeo::message::parse_headers(frame_type, sv, limits);
        if (!result) {
            throw std::invalid_argument(
                std::string(culpeo::message::error_message(result.error())));
        }
        const auto& v = *result;
        frame_type_   = frame_type;

        // Copy all present well-known headers — deviation from C++ zero-copy
        // (see DECISIONS.md §Python zero-copy deviation).
        auto put = [&](const char* name, const std::optional<std::string_view>& val) {
            if (val) headers_.emplace(name, std::string(*val));
        };
        put("Event",         v.event);
        put("Content-Type",  v.content_type);
        put("Authorization", v.authorization);
        put("Session-Id",    v.session_id);
        put("Stream-Id",     v.stream_id);
        put("Offset",        v.offset);
        put("Timestamp",     v.timestamp);
        put("Buffer-Window", v.buffer_window);
        put("Reason",        v.reason);
        put("Code",          v.code);

        body_ = std::string(v.body);
    }

    culpeo::message::FrameType type() const noexcept { return frame_type_; }

    py::dict headers() const {
        py::dict d;
        for (const auto& [k, val] : headers_) {
            d[k.c_str()] = val;
        }
        return d;
    }

    py::bytes body() const {
        return py::bytes(body_.data(), body_.size());
    }

private:
    culpeo::message::FrameType                  frame_type_{};
    std::unordered_map<std::string, std::string> headers_;
    std::string                                  body_;
};

// ─── WsTransport (Python-callback-backed) ─────────────────────────────────────
//
// Wraps culpeo::transport::WsTransport with Python-callable send/close
// functions. Each callback re-acquires the GIL (py::gil_scoped_acquire)
// since it may be invoked from any C++ thread.

class PyWsTransport {
public:
    PyWsTransport(py::object on_send_text,
                  py::object on_send_binary,
                  py::object on_close) {
        // Build C++ lambdas that re-acquire the GIL before calling Python.
        auto text_cb = [cb = std::move(on_send_text)](std::span<const std::byte> frame) {
            py::gil_scoped_acquire acquire;
            cb(py::bytes(reinterpret_cast<const char*>(frame.data()), frame.size()));
        };

        auto binary_cb = [cb = std::move(on_send_binary)](std::span<const std::byte> frame) {
            py::gil_scoped_acquire acquire;
            cb(py::bytes(reinterpret_cast<const char*>(frame.data()), frame.size()));
        };

        auto close_cb = [cb = std::move(on_close)](int code, std::string_view reason) {
            py::gil_scoped_acquire acquire;
            cb(code, std::string(reason));
        };

        inner_ = std::make_unique<culpeo::transport::WsTransport>(
            std::move(text_cb),
            std::move(binary_cb),
            std::move(close_cb));
    }

    // Non-copyable, non-movable (owns mutex via WsTransport).
    PyWsTransport(const PyWsTransport&)            = delete;
    PyWsTransport& operator=(const PyWsTransport&) = delete;

    culpeo::session::ITransport& transport() noexcept { return *inner_; }

private:
    std::unique_ptr<culpeo::transport::WsTransport> inner_;
};

// ─── Session (Python wrapper) ─────────────────────────────────────────────────
//
// Wraps culpeo::session::Session. The transport_ref_ holds a Python reference
// to the transport object (preventing GC) until the Session is destroyed.
// Session is destroyed before the transport reference is released (declaration
// order ensures session_ is listed last → destroyed first).

class PySession {
public:
    explicit PySession(py::object transport_obj,
                       py::object on_auth_validate,
                       py::object on_media_received,
                       py::object on_close,
                       py::object on_rtt,
                       py::object on_auth_response,
                       culpeo::session::SessionConfig config)
        : transport_ref_(std::move(transport_obj)) // keep transport alive
    {
        auto& transport = transport_ref_.cast<PyWsTransport&>().transport();

        culpeo::session::SessionCallbacks cbs;

        // on_auth_validate — REQUIRED. The C++ session defaults to allow-all
        // when no validator is set, which would accept every connection without
        // a token check.  Reject None here to prevent silent security bypass.
        if (on_auth_validate.is_none()) {
            throw py::value_error(
                "on_auth_validate is required; pass a callable to validate "
                "auth tokens. To explicitly allow all connections (testing "
                "only), pass: lambda token: True");
        }
        cbs.on_auth_validate = [cb = std::move(on_auth_validate)](
                                   std::string_view token) noexcept -> bool {
            try {
                py::gil_scoped_acquire acquire;
                return cb(std::string(token)).cast<bool>();
            } catch (py::error_already_set& e) {
                e.restore();
                PyErr_Print();
                return false; // deny auth on Python error
            } catch (const std::exception& e) {
                PySys_WriteStderr(
                    "culpeostream-py: on_auth_validate error: %s\n", e.what());
                return false;
            }
        };

        // on_media_received — optional
        if (!on_media_received.is_none()) {
            cbs.on_media_received = [cb = std::move(on_media_received)](
                                        const culpeo::session::StreamInfo& info,
                                        uint64_t ts_us,
                                        std::span<const std::byte> body) noexcept {
                try {
                    py::gil_scoped_acquire acquire;
                    cb(info.id,
                       ts_us,
                       py::bytes(reinterpret_cast<const char*>(body.data()),
                                  body.size()));
                } catch (py::error_already_set& e) {
                    e.restore();
                    PyErr_Print();
                } catch (const std::exception& e) {
                    PySys_WriteStderr(
                        "culpeostream-py: on_media_received error: %s\n", e.what());
                }
            };
        }

        // on_close — optional
        if (!on_close.is_none()) {
            cbs.on_close = [cb = std::move(on_close)](std::string_view code,
                                                       std::string_view reason) noexcept {
                try {
                    py::gil_scoped_acquire acquire;
                    cb(std::string(code), std::string(reason));
                } catch (py::error_already_set& e) {
                    e.restore();
                    PyErr_Print();
                } catch (const std::exception& e) {
                    PySys_WriteStderr(
                        "culpeostream-py: on_close error: %s\n", e.what());
                }
            };
        }

        // on_rtt — optional
        if (!on_rtt.is_none()) {
            cbs.on_rtt = [cb = std::move(on_rtt)](std::chrono::microseconds rtt) noexcept {
                try {
                    py::gil_scoped_acquire acquire;
                    cb(rtt.count());
                } catch (py::error_already_set& e) {
                    e.restore();
                    PyErr_Print();
                } catch (const std::exception& e) {
                    PySys_WriteStderr(
                        "culpeostream-py: on_rtt error: %s\n", e.what());
                }
            };
        }

        // on_auth_response — required for auth-refresh flow
        if (!on_auth_response.is_none()) {
            cbs.on_auth_response = [cb = std::move(on_auth_response)](
                                       std::string_view token) noexcept -> bool {
                try {
                    py::gil_scoped_acquire acquire;
                    return cb(std::string(token)).cast<bool>();
                } catch (py::error_already_set& e) {
                    e.restore();
                    PyErr_Print();
                    return false; // deny refresh on Python error
                } catch (const std::exception& e) {
                    PySys_WriteStderr(
                        "culpeostream-py: on_auth_response error: %s\n", e.what());
                    return false;
                }
            };
        }

        // Session declared last — destroyed before transport_ref_.
        session_ = std::make_unique<culpeo::session::Session>(
            transport, std::move(cbs), std::move(config));
    }

    ~PySession() {
        // Explicitly destroy session before transport_ref_ is released.
        session_.reset();
    }

    culpeo::session::SessionState state() const noexcept {
        return session_->state();
    }

    std::optional<std::string> session_id() const noexcept {
        return session_->session_id();
    }

    // Feed a raw control frame (bytes → parse → process).
    // Releases the GIL during frame processing (which may invoke send_text).
    void process_control_frame(py::bytes data) {
        // Copy bytes while GIL is held (the SV is valid for the duration).
        std::string raw(bytes_sv(data));
        std::expected<void, culpeo::session::Error> result;
        {
            // Parse and process; SV into raw (C++ std::string, GIL-independent).
            auto parsed = culpeo::message::parse_headers(
                culpeo::message::FrameType::control,
                std::string_view(raw));
            if (!parsed) {
                throw std::invalid_argument(
                    std::string(culpeo::message::error_message(parsed.error())));
            }
            py::gil_scoped_release release;
            result = session_->process_control_frame(*parsed);
        }
        if (!result) throw_session_error(result.error());
    }

    // Feed a raw media frame.
    void process_media_frame(py::bytes data) {
        std::string raw(bytes_sv(data));
        std::expected<void, culpeo::session::Error> result;
        {
            auto parsed = culpeo::message::parse_headers(
                culpeo::message::FrameType::media,
                std::string_view(raw));
            if (!parsed) {
                throw std::invalid_argument(
                    std::string(culpeo::message::error_message(parsed.error())));
            }
            py::gil_scoped_release release;
            result = session_->process_media_frame(*parsed);
        }
        if (!result) throw_session_error(result.error());
    }

    // Send media on an output/duplex stream.
    void send_media(std::string stream_id, py::bytes payload, uint64_t timestamp_us) {
        auto vec = bytes_to_vec(payload); // copy while GIL is held
        std::expected<void, culpeo::session::Error> result;
        {
            py::gil_scoped_release release;
            result = session_->send_media(
                stream_id,
                {vec.data(), vec.size()},
                timestamp_us);
        }
        if (!result) throw_session_error(result.error());
    }

    // Issue a ping.
    void send_ping() {
        std::expected<void, culpeo::session::Error> result;
        {
            py::gil_scoped_release release;
            result = session_->send_ping();
        }
        if (!result) throw_session_error(result.error());
    }

    // Initiate graceful close.
    void close(std::string code, std::string reason) {
        py::gil_scoped_release release;
        session_->close(code, reason);
    }

private:
    py::object                                   transport_ref_; // keeps transport alive
    std::unique_ptr<culpeo::session::Session>    session_;       // destroyed before transport_ref_
};

// ─── H2Client ─────────────────────────────────────────────────────────────────
//
// Synchronous Python wrapper around CulpeoH2Client.
// An asio::io_context runs in a background thread; each public method posts
// the relevant coroutine and blocks (with GIL released) on a std::future.

using WorkGuard = asio::executor_work_guard<asio::io_context::executor_type>;

class PyH2Client {
public:
    explicit PyH2Client(bool cleartext = true) {
        if (cleartext) {
            client_ = std::make_unique<culpeo::h2::CulpeoH2Client>(
                ioc_, culpeo::h2::CulpeoH2Client::AllowCleartext{});
        } else {
            ssl_ctx_.emplace(asio::ssl::context::tls_client);
            ssl_ctx_->set_verify_mode(asio::ssl::verify_peer);
            ssl_ctx_->set_default_verify_paths();
            client_ = std::make_unique<culpeo::h2::CulpeoH2Client>(ioc_, *ssl_ctx_);
        }
        // Start io_context in background.  work_ prevents it from exiting
        // when there are no pending operations.
        // NOTE: executor_work_guard is not move-assignable so we heap-allocate.
        work_ = std::make_unique<WorkGuard>(asio::make_work_guard(ioc_));
        io_thread_ = std::thread([this] { ioc_.run(); });
    }

    ~PyH2Client() {
        // Release GIL while joining the background thread so other Python
        // threads can run during teardown.
        py::gil_scoped_release release;
        client_.reset(); // trigger close if needed
        work_.reset();   // let ioc_.run() return
        if (io_thread_.joinable()) io_thread_.join();
    }

    void connect(std::string host, std::string port, std::string path) {
        spawn_void(client_->connect(
            std::move(host), std::move(port), std::move(path)));
    }

    void send_text(py::bytes data) {
        auto vec = bytes_to_vec(data);
        spawn_void(client_->transport().send_text({vec.data(), vec.size()}));
    }

    void send_binary(py::bytes data) {
        auto vec = bytes_to_vec(data);
        spawn_void(client_->transport().send_binary({vec.data(), vec.size()}));
    }

    py::tuple receive_frame() {
        auto [type, payload] = spawn_result(client_->receive_frame());
        return py::make_tuple(
            static_cast<int>(type),
            py::bytes(reinterpret_cast<const char*>(payload.data()),
                       payload.size()));
    }

    void close_session() { spawn_void(client_->close_session()); }

private:
    asio::io_context                              ioc_;
    std::unique_ptr<WorkGuard>                    work_;
    std::optional<asio::ssl::context>             ssl_ctx_;
    std::unique_ptr<culpeo::h2::CulpeoH2Client>  client_;
    std::thread                                   io_thread_;

    // Post a void coroutine, wait for completion with GIL released.
    void spawn_void(asio::awaitable<void> coro) {
        auto fut = asio::co_spawn(ioc_, std::move(coro), asio::use_future);
        py::gil_scoped_release release;
        fut.get(); // re-throws on exception (pybind11 translates std::exception)
    }

    // Post a coroutine returning T, wait with GIL released.
    template <typename T>
    T spawn_result(asio::awaitable<T> coro) {
        auto fut = asio::co_spawn(ioc_, std::move(coro), asio::use_future);
        py::gil_scoped_release release;
        return fut.get();
    }
};

// ─── H2Server ─────────────────────────────────────────────────────────────────
//
// Each accepted CulpeoStream session is exposed to Python via a
// PyServerTransport object.  The C++ ISessionHandler coroutine reads frames
// and pushes them into a thread-safe queue; Python calls receive_frame() to
// pop frames from the queue.  Python calls send_text/send_binary to post
// sends back to the io_context.
//
// Lifetime contract (Finding 2 fix):
//   • transport_ and ioc_ are raw pointers valid only while handle() runs.
//   • send_mutex_ guards the check+use of transport_/ioc_ atomically,
//     preventing TOCTOU races between validity check and pointer dereference.
//   • set_eof() nulls out transport_ and ioc_ under send_mutex_ before
//     closing the rx_queue_, so no use-after-free is possible even if Python
//     holds a reference to a PyServerTransport after PyH2Server is destroyed.

class PyServerTransport : public std::enable_shared_from_this<PyServerTransport> {
public:
    PyServerTransport(culpeo::IAsyncTransport* t, asio::io_context* ioc)
        : transport_(t), ioc_(ioc) {}

    // Called from the C++ coroutine thread to enqueue a received frame.
    void push_frame(uint8_t type, std::vector<std::byte> data) {
        rx_queue_.push({type, std::move(data)});
    }

    // Called from the C++ coroutine thread when the session ends.
    // Nulls out the raw pointers under send_mutex_ to prevent use-after-free,
    // then closes the rx_queue_ to unblock any waiting receive_frame().
    void set_eof() {
        {
            std::lock_guard<std::mutex> lk(send_mutex_);
            transport_ = nullptr;
            ioc_       = nullptr;
        }
        rx_queue_.close();
    }

    // Python: blocks until a frame arrives (GIL released).
    py::tuple receive_frame() {
        std::optional<std::pair<uint8_t, std::vector<std::byte>>> item;
        {
            py::gil_scoped_release release;
            item = rx_queue_.pop();
        }
        if (!item) {
            throw py::stop_iteration("session closed");
        }
        return py::make_tuple(
            static_cast<int>(item->first),
            py::bytes(reinterpret_cast<const char*>(item->second.data()),
                       item->second.size()));
    }

    // Python: send a control frame (blocks until sent, GIL released).
    // Atomically checks validity and captures the awaitable+ioc under
    // send_mutex_ to eliminate the TOCTOU race (Finding 2 fix).
    void send_text(py::bytes data) {
        auto vec = bytes_to_vec(data);
        asio::io_context* ioc_ptr = nullptr;
        std::optional<asio::awaitable<void>> coro;
        {
            std::lock_guard<std::mutex> lk(send_mutex_);
            if (!transport_ || !ioc_) {
                throw std::runtime_error("session transport has been closed");
            }
            // Obtain the lazy awaitable while the pointer is guaranteed live.
            coro    = transport_->send_text({vec.data(), vec.size()});
            ioc_ptr = ioc_;
        }
        auto fut = asio::co_spawn(*ioc_ptr, std::move(*coro), asio::use_future);
        py::gil_scoped_release release;
        fut.get();
    }

    // Python: send a media frame.
    void send_binary(py::bytes data) {
        auto vec = bytes_to_vec(data);
        asio::io_context* ioc_ptr = nullptr;
        std::optional<asio::awaitable<void>> coro;
        {
            std::lock_guard<std::mutex> lk(send_mutex_);
            if (!transport_ || !ioc_) {
                throw std::runtime_error("session transport has been closed");
            }
            coro    = transport_->send_binary({vec.data(), vec.size()});
            ioc_ptr = ioc_;
        }
        auto fut = asio::co_spawn(*ioc_ptr, std::move(*coro), asio::use_future);
        py::gil_scoped_release release;
        fut.get();
    }

    // Python: close the transport.
    void close_transport(int code, std::string reason) {
        asio::io_context* ioc_ptr = nullptr;
        std::optional<asio::awaitable<void>> coro;
        {
            std::lock_guard<std::mutex> lk(send_mutex_);
            if (!transport_ || !ioc_) {
                throw std::runtime_error("session transport has been closed");
            }
            coro    = transport_->close(code, std::move(reason));
            ioc_ptr = ioc_;
        }
        auto fut = asio::co_spawn(*ioc_ptr, std::move(*coro), asio::use_future);
        py::gil_scoped_release release;
        fut.get();
    }

    bool is_valid() const noexcept {
        std::lock_guard<std::mutex> lk(send_mutex_);
        return transport_ != nullptr;
    }

private:
    // Protects transport_, ioc_ check+use atomically (Finding 2 fix).
    // Reason for mutex: compound check+use — atomic alone cannot protect the
    // pointer dereference that follows the validity check.
    mutable std::mutex                                                send_mutex_;
    culpeo::IAsyncTransport*                                          transport_;
    asio::io_context*                                                 ioc_;
    SafeQueue<std::pair<uint8_t, std::vector<std::byte>>>             rx_queue_;
};

// ISessionHandler implementation: bridges the C++ coroutine API to Python.
class PyH2SessionHandler : public culpeo::h2::ISessionHandler {
public:
    explicit PyH2SessionHandler(
        SafeQueue<std::shared_ptr<PyServerTransport>>& q,
        asio::io_context& ioc)
        : session_queue_(q), ioc_(ioc) {}

    asio::awaitable<void> handle(culpeo::IAsyncTransport& transport) override {
        auto wrapper = std::make_shared<PyServerTransport>(&transport, &ioc_);
        session_queue_.push(wrapper);
        try {
            while (true) {
                auto [type, data] = co_await transport.receive_frame();
                wrapper->push_frame(type, std::move(data));
            }
        } catch (...) {
            // Connection closed or error — signal Python side.
            wrapper->set_eof();
        }
    }

private:
    SafeQueue<std::shared_ptr<PyServerTransport>>& session_queue_;
    asio::io_context&                              ioc_;
};

class PyH2Server {
public:
    explicit PyH2Server(uint16_t port, bool cleartext = true) {
        auto handler = std::make_shared<PyH2SessionHandler>(session_queue_, ioc_);
        if (cleartext) {
            server_ = std::make_unique<culpeo::h2::CulpeoH2Server>(
                ioc_,
                culpeo::h2::CulpeoH2Server::AllowCleartext{},
                port,
                handler);
        } else {
            throw std::runtime_error(
                "TLS H2Server requires cert/key; use cleartext=True for tests");
        }
        // NOTE: executor_work_guard is not move-assignable so we heap-allocate.
        work_ = std::make_unique<WorkGuard>(asio::make_work_guard(ioc_));
        // Start server accept loop.
        asio::co_spawn(ioc_, server_->run(), asio::detached);
        io_thread_ = std::thread([this] { ioc_.run(); });
    }

    ~PyH2Server() {
        py::gil_scoped_release release;
        server_->stop();
        session_queue_.close();
        work_.reset();
        if (io_thread_.joinable()) io_thread_.join();
    }

    uint16_t port() const noexcept { return server_->port(); }

    // Block until the next client session connects, then return its transport.
    // Returns None when the server is stopped.
    py::object accept() {
        std::optional<std::shared_ptr<PyServerTransport>> item;
        {
            py::gil_scoped_release release;
            item = session_queue_.pop();
        }
        if (!item) return py::none();
        return py::cast(*item);
    }

    void stop() {
        py::gil_scoped_release release;
        server_->stop();
        session_queue_.close();
    }

private:
    asio::io_context                              ioc_;
    std::unique_ptr<WorkGuard>                    work_;
    std::unique_ptr<culpeo::h2::CulpeoH2Server>  server_;
    SafeQueue<std::shared_ptr<PyServerTransport>> session_queue_;
    std::thread                                   io_thread_;
};

// ─── Module definition ────────────────────────────────────────────────────────

PYBIND11_MODULE(culpeostream, m) {
    m.doc() = "CulpeoStream C++ core library Python bindings";

    // ── Enums ─────────────────────────────────────────────────────────────────

    py::enum_<culpeo::message::FrameType>(m, "FrameType",
        "Frame type: control (text WebSocket frame) or media (binary frame).")
        .value("control", culpeo::message::FrameType::control,
               "Control frame (text); carries events, init, ping, etc.")
        .value("media",   culpeo::message::FrameType::media,
               "Media frame (binary); carries audio/video payload.")
        .export_values();

    py::enum_<culpeo::session::OffsetType>(m, "OffsetType",
        "Stream offset increment strategy (spec §5.5).")
        .value("time",    culpeo::session::OffsetType::time,    "PCM sample-count offset.")
        .value("byte",    culpeo::session::OffsetType::byte,    "Raw byte-length offset.")
        .value("message", culpeo::session::OffsetType::message, "Per-frame increment.")
        .export_values();

    py::enum_<culpeo::session::SessionState>(m, "SessionState",
        "Session lifecycle state machine states.")
        .value("uninitialized", culpeo::session::SessionState::uninitialized)
        .value("initializing",  culpeo::session::SessionState::initializing)
        .value("established",   culpeo::session::SessionState::established)
        .value("closed",        culpeo::session::SessionState::closed)
        .export_values();

    // ── ParseLimits ───────────────────────────────────────────────────────────

    py::class_<culpeo::message::ParseLimits>(m, "ParseLimits",
        "Configurable limits for the frame parser (header block, value, count).")
        .def(py::init<>())
        .def_readwrite("max_header_block_bytes",
                       &culpeo::message::ParseLimits::max_header_block_bytes)
        .def_readwrite("max_header_value_bytes",
                       &culpeo::message::ParseLimits::max_header_value_bytes)
        .def_readwrite("max_header_count",
                       &culpeo::message::ParseLimits::max_header_count);

    // ── CulpeoMessage ─────────────────────────────────────────────────────────

    py::class_<PyCulpeoMessage>(m, "CulpeoMessage",
        R"(Parsed CulpeoStream frame.

        Construct from raw bytes; the constructor parses the header block and
        body.  All header values are **copied** into Python objects (zero-copy is
        not possible across the Python/C++ boundary — see DECISIONS.md).

        Args:
            data:       Raw frame bytes (header block + CRLF CRLF + body).
            frame_type: FrameType.control (default) or FrameType.media.
            limits:     ParseLimits (optional; uses 8 KiB defaults).
        )")
        .def(py::init([](py::bytes data,
                         culpeo::message::FrameType frame_type,
                         std::optional<culpeo::message::ParseLimits> limits) {
                 return PyCulpeoMessage(data, frame_type,
                                        limits.value_or(culpeo::message::ParseLimits{}));
             }),
             py::arg("data"),
             py::arg("frame_type") = culpeo::message::FrameType::control,
             py::arg("limits")     = py::none())
        .def("type",    &PyCulpeoMessage::type,
             "Return the FrameType of this message.")
        .def("headers", &PyCulpeoMessage::headers,
             "Return a dict of all present well-known headers.")
        .def("body",    &PyCulpeoMessage::body,
             "Return the message body as bytes.");

    // ── WsTransport ───────────────────────────────────────────────────────────

    py::class_<PyWsTransport>(m, "WsTransport",
        R"(Python-callback-backed WebSocket transport.

        Wraps culpeo::transport::WsTransport with injected Python callables for
        send_text, send_binary, and close.  Use this as the transport argument
        to Session.  All callbacks are invoked with the GIL acquired.

        Args:
            on_send_text:   Callable[bytes] — called when a control frame is sent.
            on_send_binary: Callable[bytes] — called when a media frame is sent.
            on_close:       Callable[int, str] — called with (code, reason) on close.
        )")
        .def(py::init<py::object, py::object, py::object>(),
             py::arg("on_send_text"),
             py::arg("on_send_binary"),
             py::arg("on_close"));

    // ── SessionConfig ─────────────────────────────────────────────────────────

    py::class_<culpeo::session::SessionConfig>(m, "SessionConfig",
        "Protocol configuration for a session.")
        .def(py::init<>())
        .def_readwrite("max_buffer_window_ms",
                       &culpeo::session::SessionConfig::max_buffer_window_ms)
        .def_readwrite("default_buffer_window_ms",
                       &culpeo::session::SessionConfig::default_buffer_window_ms)
        .def_readwrite("max_streams",
                       &culpeo::session::SessionConfig::max_streams)
        .def_readwrite("max_ping_rate_per_second",
                       &culpeo::session::SessionConfig::max_ping_rate_per_second)
        .def_readwrite("auth_refresh_timeout_s",
                       &culpeo::session::SessionConfig::auth_refresh_timeout_s)
        .def_readwrite("supported_versions",
                       &culpeo::session::SessionConfig::supported_versions);

    // ── Session ───────────────────────────────────────────────────────────────

    py::class_<PySession>(m, "Session",
        R"(CulpeoStream server-side session.

        Manages the session lifecycle (uninitialized → initializing →
        established → closed).  Thread-safe: all methods may be called from
        concurrent Python threads.

        Args:
            transport:          WsTransport instance (must outlive this Session).
            on_auth_validate:   Callable[str] -> bool   (required for auth).
            on_media_received:  Callable[str, int, bytes] (optional; stream_id,
                                timestamp_us, body).
            on_close:           Callable[str, str] (optional; code, reason).
            on_rtt:             Callable[int] (optional; rtt_microseconds).
            on_auth_response:   Callable[str] -> bool   (required for refresh).
            config:             SessionConfig (optional).
        )")
        .def(py::init<py::object,
                      py::object,
                      py::object,
                      py::object,
                      py::object,
                      py::object,
                      culpeo::session::SessionConfig>(),
             py::arg("transport"),
             py::arg("on_auth_validate")  = py::none(),
             py::arg("on_media_received") = py::none(),
             py::arg("on_close")          = py::none(),
             py::arg("on_rtt")            = py::none(),
             py::arg("on_auth_response")  = py::none(),
             py::arg("config")            = culpeo::session::SessionConfig{})
        .def("state",      &PySession::state,
             "Return the current SessionState.")
        .def("session_id", &PySession::session_id,
             "Return the assigned session ID string (after established), or None.")
        .def("process_control_frame", &PySession::process_control_frame,
             py::arg("data"),
             "Feed a raw control frame (bytes). Parses headers + processes event.")
        .def("process_media_frame", &PySession::process_media_frame,
             py::arg("data"),
             "Feed a raw media frame (bytes). Validates stream and advances offset.")
        .def("send_media", &PySession::send_media,
             py::arg("stream_id"), py::arg("payload"), py::arg("timestamp_us") = 0,
             "Send a media payload on a server-output/duplex stream.")
        .def("send_ping",  &PySession::send_ping,
             "Send a culpeo.ping to measure RTT.")
        .def("close",      &PySession::close,
             py::arg("code") = "normal", py::arg("reason") = "Closing",
             "Initiate graceful session close.");

    // ── H2Client ──────────────────────────────────────────────────────────────

    py::class_<PyH2Client>(m, "H2Client",
        R"(HTTP/2 CulpeoStream client.

        All I/O methods are synchronous (blocking) and release the GIL while
        waiting so they are compatible with multi-threaded Python programs.

        Args:
            cleartext: If True (default), use h2c (for tests). If False, TLS
                       with system CA verification.
        )")
        .def(py::init<bool>(), py::arg("cleartext") = true)
        .def("connect", &PyH2Client::connect,
             py::arg("host"), py::arg("port"), py::arg("path") = "/",
             "Open HTTP/2 connection and submit the POST request.")
        .def("send_text", &PyH2Client::send_text,
             py::arg("data"),
             "Send a control frame (type octet 0x01 prepended automatically).")
        .def("send_binary", &PyH2Client::send_binary,
             py::arg("data"),
             "Send a media frame (type octet 0x02 prepended automatically).")
        .def("receive_frame", &PyH2Client::receive_frame,
             "Block and receive the next frame. Returns (type_byte, payload).")
        .def("close_session", &PyH2Client::close_session,
             "Send GOAWAY and close the HTTP/2 connection.");

    // ── H2Server / PyServerTransport ─────────────────────────────────────────

    py::class_<PyServerTransport, std::shared_ptr<PyServerTransport>>(
        m, "ServerTransport",
        R"(Per-session bidirectional transport returned by H2Server.accept().

        Methods release the GIL while blocking on I/O.  receive_frame() raises
        StopIteration when the session closes.
        )")
        .def("receive_frame", &PyServerTransport::receive_frame,
             "Block and receive the next frame. Returns (type_byte, payload). "
             "Raises StopIteration on session close.")
        .def("send_text", &PyServerTransport::send_text,
             py::arg("data"),
             "Send a control frame.")
        .def("send_binary", &PyServerTransport::send_binary,
             py::arg("data"),
             "Send a media frame.")
        .def("close_transport", &PyServerTransport::close_transport,
             py::arg("code") = 1000, py::arg("reason") = "normal",
             "Close the HTTP/2 stream.")
        .def_property_readonly("is_valid", &PyServerTransport::is_valid,
             "False after the session has been closed by the remote side.");

    py::class_<PyH2Server>(m, "H2Server",
        R"(HTTP/2 CulpeoStream server.

        Accepts connections and queues sessions for Python to consume via
        accept().

        Args:
            port:      TCP port (0 = OS-assigned; query with .port after start).
            cleartext: If True (default) use h2c. TLS not yet exposed for servers.
        )")
        .def(py::init<uint16_t, bool>(),
             py::arg("port") = 0, py::arg("cleartext") = true)
        .def("port", &PyH2Server::port,
             "Return the bound TCP port.")
        .def("accept", &PyH2Server::accept,
             "Block until the next client session arrives. Returns a "
             "ServerTransport or None if the server has been stopped.")
        .def("stop", &PyH2Server::stop,
             "Stop accepting new connections and unblock any pending accept().");
}
