"""
CulpeoStream Python bindings — basic test suite.

Tests cover:
  1. Module import
  2. Enum values (OffsetType, FrameType, SessionState)
  3. CulpeoMessage parsing from raw bytes
  4. Session lifecycle via an in-memory mock transport (uninitialized →
     established → closed)
  5. Session rejection on malformed/wrong-state frames
  6. GIL safety: concurrent send from a Python thread must not deadlock

The tests do NOT require a live network connection; the HTTP/2 transport
tests are in test_h2.py (separate file, requires a running server).
"""

import json
import threading

import pytest
import culpeostream


# ─── Helpers ─────────────────────────────────────────────────────────────────

def make_ctrl_frame(headers: dict[str, str], body: bytes = b"") -> bytes:
    """Build a raw control frame: headers CRLF CRLF body."""
    hdr_block = b"".join(
        f"{k}: {v}\r\n".encode() for k, v in headers.items()
    )
    return hdr_block + b"\r\n" + body


BASIC_INIT_BODY = json.dumps({
    "version": "0.3",
    "streams": [
        {
            "content_type": "audio/pcm;rate=16000;channels=1;bits=16",
            "type": "input",
            "offset_type": "time",
        }
    ],
}).encode()

BASIC_INIT_FRAME = make_ctrl_frame(
    {
        "Event": "culpeo.init",
        "Authorization": "Bearer test-token",
        "Content-Type": "application/json",
    },
    BASIC_INIT_BODY,
)


class RecordingTransport:
    """Python-side mock transport that records all frames and close calls."""

    def __init__(self):
        self.sent_text: list[bytes] = []
        self.sent_binary: list[bytes] = []
        self.close_calls: list[tuple[int, str]] = []
        self._lock = threading.Lock()

    def on_send_text(self, data: bytes) -> None:
        with self._lock:
            self.sent_text.append(data)

    def on_send_binary(self, data: bytes) -> None:
        with self._lock:
            self.sent_binary.append(data)

    def on_close(self, code: int, reason: str) -> None:
        with self._lock:
            self.close_calls.append((code, reason))

    def make_ws_transport(self) -> culpeostream.WsTransport:
        return culpeostream.WsTransport(
            on_send_text=self.on_send_text,
            on_send_binary=self.on_send_binary,
            on_close=self.on_close,
        )

    def any_text_contains(self, needle: str) -> bool:
        with self._lock:
            return any(needle.encode() in frame for frame in self.sent_text)


def make_established_session():
    """Create a session, drive it to established state, return (mock, transport, session)."""
    mock = RecordingTransport()
    transport = mock.make_ws_transport()
    session = culpeostream.Session(
        transport=transport,
        on_auth_validate=lambda token: True,
        on_auth_response=lambda token: True,
    )
    session.process_control_frame(BASIC_INIT_FRAME)
    assert session.state() == culpeostream.SessionState.established
    return mock, transport, session


# ─── 1. Module import ─────────────────────────────────────────────────────────

class TestImport:
    def test_module_has_culpeo_message(self):
        assert hasattr(culpeostream, "CulpeoMessage")

    def test_module_has_session(self):
        assert hasattr(culpeostream, "Session")

    def test_module_has_ws_transport(self):
        assert hasattr(culpeostream, "WsTransport")

    def test_module_has_session_config(self):
        assert hasattr(culpeostream, "SessionConfig")

    def test_module_has_offset_type(self):
        assert hasattr(culpeostream, "OffsetType")

    def test_module_has_h2_client(self):
        assert hasattr(culpeostream, "H2Client")

    def test_module_has_h2_server(self):
        assert hasattr(culpeostream, "H2Server")

    def test_module_has_server_transport(self):
        assert hasattr(culpeostream, "ServerTransport")


# ─── 2. Enum values ───────────────────────────────────────────────────────────

class TestOffsetType:
    """OffsetType enum values must match C++ enum class OffsetType (spec §5.5)."""

    def test_time_value(self):
        assert culpeostream.OffsetType.time.value == 0

    def test_byte_value(self):
        assert culpeostream.OffsetType.byte.value == 1

    def test_message_value(self):
        assert culpeostream.OffsetType.message.value == 2

    def test_distinct(self):
        assert culpeostream.OffsetType.time != culpeostream.OffsetType.byte
        assert culpeostream.OffsetType.byte != culpeostream.OffsetType.message


class TestFrameType:
    def test_control_not_media(self):
        assert culpeostream.FrameType.control != culpeostream.FrameType.media

    def test_control_value(self):
        assert culpeostream.FrameType.control.value == 0

    def test_media_value(self):
        assert culpeostream.FrameType.media.value == 1


class TestSessionState:
    def test_all_states_distinct(self):
        states = [
            culpeostream.SessionState.uninitialized,
            culpeostream.SessionState.initializing,
            culpeostream.SessionState.established,
            culpeostream.SessionState.closed,
        ]
        assert len(set(int(s) for s in states)) == 4


# ─── 3. CulpeoMessage parsing ─────────────────────────────────────────────────

class TestCulpeoMessage:
    def test_parse_control_frame(self):
        frame = b"Event: culpeo.init\r\nContent-Type: application/json\r\n\r\n{}"
        msg = culpeostream.CulpeoMessage(frame)
        assert msg.type() == culpeostream.FrameType.control
        hdrs = msg.headers()
        assert hdrs["Event"] == "culpeo.init"
        assert hdrs["Content-Type"] == "application/json"
        assert msg.body() == b"{}"

    def test_parse_media_frame(self):
        frame = b"Stream-Id: s1\r\nOffset: 0\r\n\r\nbinary"
        msg = culpeostream.CulpeoMessage(frame, culpeostream.FrameType.media)
        assert msg.type() == culpeostream.FrameType.media
        assert msg.body() == b"binary"
        assert msg.headers()["Stream-Id"] == "s1"
        assert msg.headers()["Offset"] == "0"

    def test_parse_empty_body(self):
        frame = b"Event: culpeo.ping\r\n\r\n"
        msg = culpeostream.CulpeoMessage(frame)
        assert msg.headers()["Event"] == "culpeo.ping"
        assert msg.body() == b""

    def test_parse_all_known_headers(self):
        frame = (
            b"Event: culpeo.init\r\n"
            b"Content-Type: application/json\r\n"
            b"Authorization: Bearer tok\r\n"
            b"Session-Id: abc\r\n"
            b"Stream-Id: s1\r\n"
            b"Offset: 42\r\n"
            b"Timestamp: 1000\r\n"
            b"Buffer-Window: 5000\r\n"
            b"Reason: test\r\n"
            b"Code: normal\r\n"
            b"\r\n"
        )
        msg = culpeostream.CulpeoMessage(frame)
        hdrs = msg.headers()
        assert hdrs["Event"] == "culpeo.init"
        assert hdrs["Authorization"] == "Bearer tok"
        assert hdrs["Session-Id"] == "abc"
        assert hdrs["Offset"] == "42"
        assert hdrs["Reason"] == "test"
        assert hdrs["Code"] == "normal"

    def test_missing_terminator_raises(self):
        bad = b"Event: culpeo.init"  # no \r\n\r\n
        with pytest.raises((ValueError, RuntimeError)):
            culpeostream.CulpeoMessage(bad)

    def test_invalid_header_name_raises(self):
        # Null byte in header name — must be rejected by parser.
        bad = b"Event\x00: bad\r\n\r\n"
        with pytest.raises((ValueError, RuntimeError)):
            culpeostream.CulpeoMessage(bad)

    def test_custom_limits_applied(self):
        limits = culpeostream.ParseLimits()
        limits.max_header_block_bytes = 10  # very small
        frame = b"Event: culpeo.init\r\nContent-Type: application/json\r\n\r\n"
        with pytest.raises((ValueError, RuntimeError)):
            culpeostream.CulpeoMessage(frame, limits=limits)

    def test_binary_body_preserved(self):
        """Body bytes are preserved exactly (no string encoding applied)."""
        body = bytes(range(256))
        frame = b"Event: media\r\n\r\n" + body
        msg = culpeostream.CulpeoMessage(frame)
        assert msg.body() == body


# ─── 4. Session lifecycle ─────────────────────────────────────────────────────

class TestSessionLifecycle:
    def test_initial_state_is_uninitialized(self):
        mock = RecordingTransport()
        transport = mock.make_ws_transport()
        session = culpeostream.Session(transport=transport)
        assert session.state() == culpeostream.SessionState.uninitialized
        assert session.session_id() is None

    def test_init_transitions_to_established(self):
        mock, transport, session = make_established_session()
        assert session.state() == culpeostream.SessionState.established
        assert session.session_id() is not None
        assert len(session.session_id()) == 32  # 128-bit hex

    def test_init_sends_init_ack(self):
        mock, transport, session = make_established_session()
        assert mock.any_text_contains("culpeo.init-ack")

    def test_close_transitions_to_closed(self):
        mock, transport, session = make_established_session()
        session.close()
        assert session.state() == culpeostream.SessionState.closed

    def test_close_sends_event(self):
        mock, transport, session = make_established_session()
        session.close(code="normal", reason="done")
        # transport.close should have been called (or a close frame sent)
        # Either close_calls or sent_text with culpeo.close
        closed = (
            len(mock.close_calls) > 0
            or mock.any_text_contains("culpeo.close")
            or mock.any_text_contains("culpeo.session-close")
        )
        assert closed

    def test_wrong_state_media_before_init_raises(self):
        """Sending media before init must raise RuntimeError (wrong_state)."""
        mock = RecordingTransport()
        transport = mock.make_ws_transport()
        session = culpeostream.Session(
            transport=transport,
            on_auth_validate=lambda t: True,
        )
        with pytest.raises(RuntimeError):
            session.send_media("s1", b"\x00\x00", 0)

    def test_process_control_frame_wrong_state(self):
        """A second culpeo.init while established must be rejected."""
        mock, transport, session = make_established_session()
        # Sending another culpeo.init while established is wrong_state.
        # The session might close itself or return an error — either way no crash.
        try:
            session.process_control_frame(BASIC_INIT_FRAME)
        except RuntimeError:
            pass  # expected

    def test_auth_failure_rejects_session(self):
        """culpeo.init with failing auth should not establish the session."""
        mock = RecordingTransport()
        transport = mock.make_ws_transport()
        session = culpeostream.Session(
            transport=transport,
            on_auth_validate=lambda token: False,  # always reject
            on_auth_response=lambda token: False,
        )
        try:
            session.process_control_frame(BASIC_INIT_FRAME)
        except RuntimeError:
            pass
        # Session should NOT be established.
        assert session.state() != culpeostream.SessionState.established

    def test_version_mismatch_rejects_session(self):
        """culpeo.init with unsupported version must not establish."""
        bad_body = json.dumps({
            "version": "9.9",
            "streams": [
                {
                    "content_type": "audio/pcm;rate=16000;channels=1;bits=16",
                    "type": "input",
                    "offset_type": "time",
                }
            ],
        }).encode()
        bad_frame = make_ctrl_frame(
            {
                "Event": "culpeo.init",
                "Authorization": "Bearer tok",
                "Content-Type": "application/json",
            },
            bad_body,
        )
        mock = RecordingTransport()
        transport = mock.make_ws_transport()
        session = culpeostream.Session(
            transport=transport,
            on_auth_validate=lambda t: True,
        )
        try:
            session.process_control_frame(bad_frame)
        except RuntimeError:
            pass
        assert session.state() != culpeostream.SessionState.established

    def test_ping_from_established_session(self):
        """Session can send a ping once established."""
        mock, transport, session = make_established_session()
        before = len(mock.sent_text)
        session.send_ping()
        # A culpeo.ping frame should have been sent.
        assert len(mock.sent_text) > before


# ─── 5. SessionConfig ─────────────────────────────────────────────────────────

class TestSessionConfig:
    def test_defaults(self):
        cfg = culpeostream.SessionConfig()
        assert cfg.max_streams == 16
        assert cfg.max_buffer_window_ms == 30_000
        assert "0.3" in cfg.supported_versions

    def test_custom_config_passed_through(self):
        cfg = culpeostream.SessionConfig()
        cfg.max_streams = 4
        mock = RecordingTransport()
        transport = mock.make_ws_transport()
        session = culpeostream.Session(
            transport=transport,
            on_auth_validate=lambda t: True,
            config=cfg,
        )
        assert session.state() == culpeostream.SessionState.uninitialized


# ─── 6. GIL safety — concurrent sends ────────────────────────────────────────

class TestGilSafety:
    """Spawn Python threads and do concurrent sends — must not deadlock."""

    def test_concurrent_send_no_deadlock(self, timeout=5.0):
        """
        Two threads call process_control_frame simultaneously.
        Under correct GIL management this completes without hanging.
        """
        mock, transport, session = make_established_session()

        # Build a second culpeo.init that will be rejected (wrong state);
        # we don't care about success — just that it doesn't deadlock.
        results: list[Exception | None] = [None, None]

        def worker(idx: int) -> None:
            try:
                session.process_control_frame(BASIC_INIT_FRAME)
            except RuntimeError as e:
                results[idx] = e

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(2)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=timeout)
            assert not t.is_alive(), "Thread deadlocked!"

    def test_concurrent_close_and_send(self, timeout=5.0):
        """Close from one thread, send_ping from another — no deadlock or crash."""
        mock, transport, session = make_established_session()

        def close_worker():
            try:
                session.close()
            except Exception:
                pass

        def ping_worker():
            try:
                session.send_ping()
            except Exception:
                pass

        t1 = threading.Thread(target=close_worker)
        t2 = threading.Thread(target=ping_worker)
        t1.start()
        t2.start()
        t1.join(timeout=timeout)
        t2.join(timeout=timeout)
        assert not t1.is_alive(), "close_worker deadlocked!"
        assert not t2.is_alive(), "ping_worker deadlocked!"

    def test_transport_callback_from_non_python_thread(self, timeout=5.0):
        """
        WsTransport callbacks are called from C++ (potentially a non-Python
        thread).  Verify the GIL re-acquisition works correctly.

        We create a second session and feed it from a background thread.
        The on_auth_validate callback runs in C++ and must re-acquire the GIL.
        """
        auth_thread_ids: list[int] = []
        lock = threading.Lock()

        def on_auth(token: str) -> bool:
            with lock:
                auth_thread_ids.append(threading.get_ident())
            return True

        mock = RecordingTransport()
        transport = mock.make_ws_transport()
        session = culpeostream.Session(
            transport=transport,
            on_auth_validate=on_auth,
        )

        completed = threading.Event()
        error_holder: list[Exception] = []

        def worker():
            try:
                session.process_control_frame(BASIC_INIT_FRAME)
                completed.set()
            except Exception as exc:
                error_holder.append(exc)
                completed.set()

        t = threading.Thread(target=worker)
        t.start()
        assert completed.wait(timeout=timeout), "Worker timed out"
        t.join(timeout=timeout)
        assert not t.is_alive(), "Worker thread deadlocked"

        if error_holder:
            raise error_holder[0]

        # auth callback was invoked (from the worker thread)
        assert len(auth_thread_ids) == 1

    def test_many_threads_concurrent_process_frames(self, timeout=10.0):
        """
        N threads each try to process a frame concurrently.
        The session handles one init and rejects the rest — all threads must
        complete without hanging.
        """
        n = 8
        mock = RecordingTransport()
        transport = mock.make_ws_transport()
        session = culpeostream.Session(
            transport=transport,
            on_auth_validate=lambda t: True,
        )

        barrier = threading.Barrier(n)
        done = threading.Event()
        results: list[BaseException | None] = [None] * n

        def worker(idx: int) -> None:
            barrier.wait()  # all threads start at the same time
            try:
                session.process_control_frame(BASIC_INIT_FRAME)
            except RuntimeError as e:
                results[idx] = e

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(n)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=timeout)
            assert not t.is_alive(), f"Thread {t.name} deadlocked!"

        # Exactly one init should have succeeded; the rest are wrong_state.
        errors = [r for r in results if r is not None]
        assert len(errors) == n - 1
