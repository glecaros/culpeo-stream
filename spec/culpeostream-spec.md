# CulpeoStream Protocol Specification

**Version:** 0.3.0-draft  
**Status:** Draft  
**Last Updated:** 2026-05-28

---

## Table of Contents

1. [Overview](#1-overview)
2. [Design Goals](#2-design-goals)
3. [Transport](#3-transport)
4. [Frame Format](#4-frame-format)
5. [Streams](#5-streams)
6. [Frame Types](#6-frame-types)
7. [Session Lifecycle](#7-session-lifecycle)
8. [Media Streaming](#8-media-streaming)
9. [Event System](#9-event-system)
10. [Error Handling](#10-error-handling)
11. [Addendum A: Authentication](#addendum-a-authentication)
12. [Addendum B: WebSocket Binding](#addendum-b-websocket-binding)

---

## 1. Overview

CulpeoStream is a lightweight, bidirectional streaming protocol designed for real-time AI media applications. It provides:

- A self-describing frame format with HTTP-inspired headers
- First-class support for multiplexed, multi-modal media streams
- Session resumption across connection drops with per-stream offsets and negotiated buffering
- A minimal, low-latency session establishment handshake with explicit version negotiation
- Protocol-level keepalive and latency measurement independent of the underlying transport
- An extensible event system with a reserved namespace for protocol events and an open namespace for application-defined events

CulpeoStream is transport-agnostic by design. The canonical binding is WebSocket (see Addendum B), but the frame format and session model are defined independently of any specific transport.

---

## 2. Design Goals

**Low latency.** Session establishment completes in a single round trip. The protocol introduces no per-frame overhead beyond what is necessary for routing and media synchronization.

**Resilience.** Sessions survive transient connection drops. Clients may reconnect and resume a session using a server-assigned session identifier and per-stream frame offsets, without replaying context or restarting inference.

**Multi-stream and multimodal.** A single session may carry multiple concurrent streams of different media types and directions. Streams are declared at session initialization and assigned server-managed identifiers.

**Self-describing frames.** Each frame carries its own type and metadata via human-readable headers, making frames independently parseable without external schema negotiation.

**Transport agnostic.** The core protocol — frame format, session model, stream lifecycle, event system — is defined independently of the underlying transport. Transport-specific concerns are isolated in addenda.

**Browser compatible.** The canonical WebSocket binding requires no custom HTTP upgrade headers, making CulpeoStream usable from browsers, mobile clients, and server-side runtimes without modification.

**Extensible.** Unknown headers SHOULD be ignored. Application layers may define custom events without modifying the protocol.

---

## 3. Transport

CulpeoStream is transport-agnostic. The frame format defined in Section 4 may be carried over any reliable, ordered, full-duplex byte stream or message transport. Transport-specific bindings are defined in addenda.

The following properties are assumed of any compliant transport binding:

- **Reliable delivery** — frames are delivered or the connection is considered lost.
- **Ordered delivery** — frames within a stream are delivered in the order sent.
- **Full duplex** — both parties may send frames simultaneously.
- **Frame boundary preservation** — the transport either preserves message boundaries (as WebSocket does) or the binding defines a framing layer.

The canonical binding for CulpeoStream is WebSocket, described in [Addendum B](#addendum-b-websocket-binding).

### 3.1 Encryption

All CulpeoStream transports MUST use encryption in production. For the WebSocket binding, this means `wss://`. Unencrypted transports MAY be used in local development environments only.

---

## 4. Frame Format

Every CulpeoStream frame follows the same structure regardless of transport:

```
(<header-name>: <header-value>\r\n)*
\r\n
<body>
```

- **Headers** are `\r\n`-terminated key-value pairs. Header names are case-insensitive ASCII. Header values are UTF-8 strings.
- **The header block is terminated** by a bare `\r\n` (an empty line), identical to HTTP.
- **The body** follows immediately after the header terminator. For control and event frames, the body is a UTF-8 encoded JSON object. For media frames, the body is raw bytes.

Frames are distinguished by a `Frame-Type` property communicated by the transport binding. In the WebSocket binding, WebSocket text frames carry control and event messages, and WebSocket binary frames carry media payloads (see Addendum B).

### 4.1 Header Syntax

```
header-field = field-name ":" OWS field-value OWS CRLF
field-name   = token          ; case-insensitive ASCII, no leading/trailing whitespace
field-value  = *( VCHAR / SP )
OWS          = *( SP / HTAB )  ; optional whitespace
```

Implementations MUST parse the header value by stripping leading and trailing `SP` and `HTAB` characters (optional whitespace). The colon separator MAY be followed by zero or more `SP` or `HTAB` characters before the field value begins. Header names MUST NOT contain leading or trailing whitespace; implementations MUST NOT trim or normalize header names beyond case-insensitive matching of reserved headers.

Header names follow the same token rules as HTTP/1.1. Implementations MUST ignore unknown headers. Duplicate headers with the same name MUST NOT appear in a frame. If a frame contains duplicate headers, the implementation MUST reject the frame as a protocol error. The order of headers within a frame is not significant; implementations MUST NOT depend on header ordering for correctness.

Header names and values MUST NOT contain CR (`\r`), LF (`\n`), or NUL (`\0`) bytes. Implementations MUST reject any frame containing these bytes in a header field name or value as a protocol error.

### 4.1.1 Parser Limits

Implementations MUST enforce the following limits during header parsing. A frame that exceeds any limit MUST be rejected before further buffering or processing.

| Limit | Minimum Required | Recommended Default |
|---|---|---|
| Maximum header block size (bytes) | 8,192 | 8,192 |
| Maximum number of headers per frame | 64 | 64 |
| Maximum individual header name length (bytes) | 256 | 256 |
| Maximum individual header value length (bytes) | 4,096 | 4,096 |

Implementations MAY allow operators to configure higher limits but MUST NOT default to unlimited. If the header block terminator (`\r\n\r\n`) is not found within the maximum header block size, the implementation MUST reject the frame without buffering additional data.

### 4.2 Reserved Headers

The following headers are defined by this specification:

| Header | Applicable Frames | Description |
|---|---|---|
| `Event` | Control / event | The event type. Required on all control and event frames. |
| `Content-Type` | All | Media type of the body. Required when body is non-empty. |
| `Authorization` | `culpeo.init`, `culpeo.auth-response` | Credentials for session authentication. See Addendum A. |
| `Session-Id` | `culpeo.init-ack`, `culpeo.init` (resume) | Server-assigned session identifier. |
| `Stream-Id` | Media, stream-scoped events | Identifies the stream this frame belongs to. |
| `Offset` | Media, `culpeo.init-ack` (resume) | Frame sequence offset within the stream. See Section 8.2. |
| `Timestamp` | Media | Presentation timestamp in microseconds since session start. |
| `Buffer-Window` | `culpeo.init`, `culpeo.init-ack` | Requested or confirmed resumption buffer in milliseconds. |
| `Reason` | `culpeo.init-error`, `culpeo.close` | Human-readable description of an error or close condition. |
| `Code` | `culpeo.init-error`, `culpeo.close` | Machine-readable error or close code. |

Integer-valued headers (`Offset`, `Timestamp`, `Buffer-Window`) MUST be serialized as base-10 decimal integers with no leading zeros, no leading `+` sign, no exponent notation, and no decimal point. Implementations MUST reject non-integer values for these headers as a protocol error.

### 4.3 Body

The body is the octets following the header terminator (`\r\n\r\n`) to the end of the frame.

- For **control and event frames**, the body MUST be a valid UTF-8 JSON object. An empty body is represented as `{}`. Serializers MUST emit `{}` when the body would otherwise be empty; an absent body is not permitted on control or event frames.
- For **media frames**, the body is the raw media payload described by the `Content-Type` header.

---

## 5. Streams

### 5.1 Overview

A CulpeoStream session carries one or more **streams**. Each stream has a declared media type, a directionality type, and an optional purpose label. Streams are declared by the client in the `culpeo.init` frame and confirmed by the server in `culpeo.init-ack` with server-assigned identifiers.

Every media frame and stream-scoped event frame carries a `Stream-Id` header identifying which stream it belongs to.

### 5.2 Stream Declaration

Each stream is declared as an object with the following fields:

| Field | Required | Description |
|---|---|---|
| `content_type` | REQUIRED | The media type of the stream payload. |
| `type` | REQUIRED | Directionality: `input`, `output`, or `duplex`. |
| `purpose` | CONDITIONAL | Semantic label for the stream. See Section 5.4. |
| `id` | OPTIONAL | Previously assigned stream ID, used as a resumption hint. |

#### Stream Types

| Type | Meaning |
|---|---|
| `input` | Client sends media; server receives. |
| `output` | Server sends media; client receives. |
| `duplex` | Both parties may send on this stream. |

Sending a media frame on a stream in a direction inconsistent with its declared `type` is a protocol error. The receiving party MUST close the session with code `protocol-error`.

### 5.3 Stream Identifiers

Stream identifiers are assigned by the server in `culpeo.init-ack`. They are opaque strings, unique within a session, and MUST be generated using a cryptographically secure random number generator.

On resumption, the client MAY include previously assigned stream IDs as hints. The server MAY honor these IDs or assign new ones. If IDs are reassigned, the client MUST use the new IDs for all subsequent frames and SHOULD match streams by `type` and `purpose` when remapping.

Resumption is all-or-nothing with respect to streams: the client MUST declare exactly the same set of streams (matched by `type`, `content_type`, and `purpose`) as the original session. If the server cannot match every declared stream to an existing stream in the session, it MUST reject the resumption with `culpeo.init-error` code `invalid-streams`. The server MUST NOT create new streams during resumption or silently drop unmatched declarations.

On a fresh session, any client-provided IDs in stream declarations MUST be ignored by the server.

### 5.4 Purpose

The `purpose` field is a free-form string identifying the semantic role of a stream within the application. It is opaque to the protocol and carries no protocol-level meaning.

**Purpose is OPTIONAL** when a session declares at most one stream per `type`.

**Purpose is REQUIRED** on all streams of a given `type` when two or more streams share that `type`. In this case, `purpose` values MUST be unique among streams of the same `type`.

A server receiving an `init` frame that violates these rules MUST respond with `culpeo.init-error` code `invalid-streams`.

**Example — valid, purpose not required:**
```json
{
  "version": "0.3",
  "streams": [
    {"content_type": "audio/pcm;rate=16000;channels=1;bits=16", "type": "input", "offset_type": "time"},
    {"content_type": "audio/opus", "type": "output", "offset_type": "message"}
  ]
}
```

**Example — valid, purpose required and unique:**
```json
{
  "version": "0.3",
  "streams": [
    {"content_type": "audio/pcm;rate=16000;channels=1;bits=16", "type": "input", "purpose": "user-voice", "offset_type": "time"},
    {"content_type": "audio/pcm;rate=44100;channels=2;bits=16", "type": "input", "purpose": "background-music", "offset_type": "time"},
    {"content_type": "audio/opus", "type": "output", "purpose": "assistant-voice", "offset_type": "message"},
    {"content_type": "application/json", "type": "duplex", "purpose": "events", "offset_type": "message"}
  ]
}
```

**Example — invalid, duplicate purpose within type:**
```json
{
  "version": "0.3",
  "streams": [
    {"content_type": "audio/pcm;rate=16000;channels=1;bits=16", "type": "input", "purpose": "user-voice", "offset_type": "time"},
    {"content_type": "audio/pcm;rate=44100;channels=2;bits=16", "type": "input", "purpose": "user-voice", "offset_type": "time"}
  ]
}
```

### 5.5 Offset Types

Each stream carries an `offset_type` field that declares how `Offset` values on media frames advance. This field is REQUIRED in stream declarations.

| Value | Offset unit | Typical use |
|---|---|---|
| `time` | Samples per channel (for PCM) or other time-unit defined by the content type | Raw PCM audio — provides a time-aligned cursor stable across reconnections regardless of frame sizing |
| `byte` | Bytes — the raw byte length of the media payload | Opaque binary streams where byte position is the natural cursor |
| `message` | 1 per delivered media frame | Encoded audio (Opus, AAC), JSON event payloads, or any stream where per-message count is the natural unit |

> **Note:** The value `message` (rather than `frame`) is used because this unit applies equally to protocol frames and higher-level application messages such as JSON payloads, where "frame" would be ambiguous.

The `offset_type` determines the increment applied after each media frame is delivered (see §8.2). Implementations MUST reject a stream declaration with an unrecognised `offset_type` as `invalid-streams`.

### 5.6 Stream Validation

A server receiving a `culpeo.init` frame MUST validate the following. Violations MUST produce `culpeo.init-error` with code `invalid-streams`:

1. At least one stream MUST be declared.
2. Each stream MUST include `content_type`, `type`, and `offset_type`.
3. `type` MUST be one of `input`, `output`, `duplex`.
4. `offset_type` MUST be one of `time`, `byte`, `message`.
5. When two or more streams share the same `type`, all streams of that `type` MUST declare a `purpose`.
6. `purpose` values MUST be unique within a `type`.

### 5.7 Stream Count Limits

Implementations MUST enforce a maximum number of streams per session. The default maximum MUST NOT exceed **16** streams. Implementations MAY allow operators to configure a higher limit.

A `culpeo.init` frame declaring more streams than the server's configured maximum MUST be rejected with `culpeo.init-error` code `invalid-streams` and reason indicating the limit was exceeded. The server SHOULD reject the frame before allocating per-stream resources for the excess streams.

---

## 6. Frame Types

### 6.1 Protocol Events

Protocol events use the `culpeo.` namespace and are defined by this specification. Implementations MUST handle all protocol events defined here. The `culpeo.` namespace MUST NOT be used by application-defined events.

---

#### `culpeo.init`

Sent by the client as the **first frame** of every connection. No other frame may be sent before receiving `culpeo.init-ack`. Any frame received by the server before `culpeo.init` MUST result in immediate connection closure with code `protocol-error`.

**New session:**
```
Event: culpeo.init
Authorization: Bearer <token>
Content-Type: application/json
Buffer-Window: <requested-ms>

{
  "version": "0.3",
  "streams": [
    {"content_type": "audio/pcm;rate=16000;channels=1;bits=16", "type": "input", "purpose": "user-voice"},
    {"content_type": "audio/opus", "type": "output", "purpose": "assistant-voice"},
    {"content_type": "application/json", "type": "duplex", "purpose": "events"}
  ]
}
```

**Resumption:**
```
Event: culpeo.init
Authorization: Bearer <token>
Content-Type: application/json
Session-Id: <previous-session-id>
Buffer-Window: <requested-ms>

{
  "version": "0.3",
  "streams": [
    {"id": "s1", "content_type": "audio/pcm;rate=16000;channels=1;bits=16", "type": "input", "purpose": "user-voice", "offset_type": "time", "resume_offset": 2048},
    {"id": "s2", "content_type": "audio/opus", "type": "output", "purpose": "assistant-voice", "offset_type": "message", "resume_offset": 312},
    {"id": "s3", "content_type": "application/json", "type": "duplex", "purpose": "events", "offset_type": "message", "resume_offset": 45}
  ]
}
```

---

#### `culpeo.init-ack`

Sent by the server upon successful `culpeo.init`. After this frame, the session is established and both parties may send media and event frames.

**New session:**
```
Event: culpeo.init-ack
Session-Id: <server-assigned-id>
Buffer-Window: <negotiated-ms>
Content-Type: application/json

{
  "version": "0.3",
  "streams": [
    {"id": "s1", "content_type": "audio/pcm;rate=16000;channels=1;bits=16", "type": "input", "purpose": "user-voice", "offset_type": "time"},
    {"id": "s2", "content_type": "audio/opus", "type": "output", "purpose": "assistant-voice", "offset_type": "message"},
    {"id": "s3", "content_type": "application/json", "type": "duplex", "purpose": "events", "offset_type": "message"}
  ]
}
```

**Resumption:** each confirmed stream includes the offset from which the server is resuming. This MAY differ from the client's requested `resume_offset` if part of the buffer has been evicted.

```
Event: culpeo.init-ack
Session-Id: <session-id>
Buffer-Window: <negotiated-ms>
Content-Type: application/json

{
  "version": "0.3",
  "streams": [
    {"id": "s1", "content_type": "audio/pcm;rate=16000;channels=1;bits=16", "type": "input", "purpose": "user-voice", "offset_type": "time", "resume_offset": 2048},
    {"id": "s2", "content_type": "audio/opus", "type": "output", "purpose": "assistant-voice", "offset_type": "message", "resume_offset": 298},
    {"id": "s3", "content_type": "application/json", "type": "duplex", "purpose": "events", "offset_type": "message", "resume_offset": 45}
  ]
}
```

---

#### `culpeo.init-error`

Sent by the server when `culpeo.init` fails. The server MUST close the connection immediately after sending this frame. The client MUST NOT retry on the same connection.

```
Event: culpeo.init-error
Code: <error-code>
Reason: <human-readable description>
Content-Type: application/json

{}
```

For `unsupported-version`, the body MUST include the versions the server supports:

```
Event: culpeo.init-error
Code: unsupported-version
Reason: Protocol version not supported
Content-Type: application/json

{"supported_versions": ["0.2", "0.3"]}
```

Defined error codes:

| Code | Meaning |
|---|---|
| `unauthorized` | The provided credentials are invalid or expired. |
| `unsupported-version` | The declared protocol version is not supported by the server. |
| `invalid-session` | The provided `Session-Id` does not exist or has expired. |
| `invalid-streams` | Stream declarations violate Section 5.5. |
| `protocol-error` | The first frame was not a valid `culpeo.init` frame. |
| `server-error` | An internal server error prevented session establishment. |

---

#### `culpeo.ping`

Sent by either party at any time after session establishment to measure round-trip latency or verify connection liveness. The receiving party MUST respond with `culpeo.pong`, subject to rate limiting.

Implementations MUST enforce a per-session ping rate limit. The default limit MUST NOT exceed **5 pings per second** per session. Pings received in excess of the rate limit SHOULD be silently dropped without sending a pong response. Implementations MUST NOT close the session solely due to excess pings.

```
Event: culpeo.ping
Content-Type: application/json

{"ts": 1716393600000000}
```

`ts` is the sender's current time in microseconds since Unix epoch.

---

#### `culpeo.pong`

Sent in response to `culpeo.ping`. The sender echoes the original `ts` and adds its own timestamp, allowing the initiating party to compute both round-trip time and a one-way latency estimate.

```
Event: culpeo.pong
Content-Type: application/json

{"ts": 1716393600000000, "server_ts": 1716393600512000}
```

`ts` is the echoed timestamp from `culpeo.ping`. `server_ts` is the responder's current time in microseconds since Unix epoch.

---

#### `culpeo.auth-refresh`

Sent by the **server** to request credential renewal. The session continues uninterrupted; media frames MAY continue to flow during the exchange.

```
Event: culpeo.auth-refresh
Content-Type: application/json

{"nonce": "<server-generated-nonce>"}
```

The server MUST NOT issue a new `culpeo.auth-refresh` while a previous challenge is outstanding (i.e., before receiving the corresponding `culpeo.auth-response` or closing the session due to timeout). A client that receives a second `culpeo.auth-refresh` while one is already pending SHOULD ignore the duplicate.

---

#### `culpeo.auth-response`

Sent by the **client** in response to `culpeo.auth-refresh`. The client MUST echo the nonce. An invalid or missing nonce MUST cause the server to close the session with code `unauthorized`.

```
Event: culpeo.auth-response
Authorization: Bearer <new-token>
Content-Type: application/json

{"nonce": "<echoed-nonce>"}
```

Clients MAY refuse to respond to `culpeo.auth-refresh` if the challenge frequency exceeds application policy. A client that chooses to refuse SHOULD close the session with code `protocol-error`.

---

#### `culpeo.close`

Sent by either party to initiate graceful session termination. The receiving party SHOULD respond with its own `culpeo.close` before closing the connection.

```
Event: culpeo.close
Code: <close-code>
Reason: <human-readable description>
Content-Type: application/json

{}
```

Defined close codes:

| Code | Meaning |
|---|---|
| `normal` | Intentional, clean session end. |
| `auth-expired` | Session closed due to failed credential refresh. |
| `server-shutdown` | Server is shutting down or restarting. |
| `idle-timeout` | Session closed due to inactivity. |
| `protocol-error` | A protocol violation was detected. |

---

### 6.2 Media Frames

Media frames carry stream payloads. In the WebSocket binding they are binary frames; other transport bindings define their own media frame representation (see Addendum B).

```
Stream-Id: <id>
Offset: <frame-offset>
Content-Type: <media-type>
Timestamp: <microseconds>

<binary payload>
```

`Stream-Id` MUST reference a stream confirmed in `culpeo.init-ack`. The `Content-Type` header on a media frame MUST match the `content_type` declared for that stream. Comparison MUST be case-insensitive for the media type and subtype portions; parameter comparison MUST be case-insensitive for parameter names and case-sensitive for parameter values. A mismatch MUST result in `culpeo.close` with code `protocol-error`.

Sending a media frame on a stream in a direction inconsistent with its declared `type` MUST result in `culpeo.close` with code `protocol-error`.

#### Defined Media Content Types

| Content-Type | Description |
|---|---|
| `audio/pcm;rate=<hz>;channels=<n>;bits=<depth>` | Raw linear PCM audio |
| `audio/opus` | Opus-encoded audio frames |
| `audio/aac` | AAC-encoded audio |

For `audio/pcm`, the parameters `rate`, `channels`, and `bits` are all REQUIRED. Implementations MUST reject a stream declaration or media frame with an `audio/pcm` content type that is missing any of these three parameters. The `rate` parameter specifies the sample rate in hertz, `channels` specifies the number of audio channels (MUST be ≥ 1), and `bits` specifies the bit depth per sample (MUST be a positive multiple of 8). Unknown parameters on `audio/pcm` MUST be ignored. Parameter order is not significant.

Implementations that receive an unknown `Content-Type` on a media frame SHOULD discard the frame and MAY send `culpeo.close` with code `protocol-error`.

---

## 7. Session Lifecycle

### 7.1 New Session

```
Client                            Server
  |                                 |
  |--- Transport Handshake -------->|
  |<-- Transport Handshake ---------|
  |                                 |
  |--- culpeo.init ---------------->|
  |    version, auth, streams       |
  |<-- culpeo.init-ack -------------|
  |    session-id, stream ids       |
  |                                 |
  |==== media / event frames =======|
  |                                 |
  |--- culpeo.close --------------->|
  |<-- culpeo.close -----------------|
  |                                 |
  X                                 X
```

### 7.2 Session Resumption

```
Client                            Server
  |                                 |
  |--- Transport Handshake -------->|
  |<-- Transport Handshake ---------|
  |                                 |
  |--- culpeo.init ---------------->|
  |    session-id, auth,            |
  |    per-stream resume offsets    |
  |<-- culpeo.init-ack -------------|
  |    confirmed offsets per stream |
  |                                 |
  |==== session resumes ============|
  |                                 |
```

If the `resume_offset` for a stream is within the server's buffer, the server resumes from that offset. If partially evicted, the server resumes from the earliest available offset for that stream and reports it in `culpeo.init-ack`. Streams whose buffer has fully expired resume from the current position with no replay.

A client MUST NOT request a `resume_offset` greater than the highest offset it has received for that stream. If a `resume_offset` exceeds the server's current offset for that stream, the server MUST reject the resumption with `culpeo.init-error` code `invalid-streams`. The server MUST clamp each accepted `resume_offset` upward to the earliest available buffered offset (i.e., `confirmed_offset = max(requested_offset, earliest_available_offset)`).

Session expiry MUST be determined by wall-clock time elapsed since disconnection. The server MUST track the time of disconnection and compare it against the negotiated `Buffer-Window` from the original session. If the elapsed time exceeds the buffer window, the session has expired.

If the session itself has expired, the server responds with `culpeo.init-error` code `invalid-session`. The client SHOULD treat this as a new session.

### 7.3 Version Negotiation

The client declares its protocol version in the `culpeo.init` body. The server confirms the version in `culpeo.init-ack`. If the server does not support the declared version, it MUST respond with `culpeo.init-error` code `unsupported-version`, include the list of supported versions in the response body, and close the connection immediately.

The client MUST NOT retry version negotiation on the same connection. It MAY open a new connection and retry with a supported version from the error response.

### 7.4 Session Expiry

Sessions are maintained server-side for the duration of the negotiated `Buffer-Window` after a connection drop. After this window elapses, the session and its buffers are discarded. The server MAY adjust the effective buffer window based on resource constraints and MUST reflect the actual window in `culpeo.init-ack`.

If the client omits the `Buffer-Window` header from `culpeo.init`, the server MUST use its configured default (up to the maximum). A `Buffer-Window` value of `0` indicates the client does not request resumption capability; the server MUST reflect `0` in `culpeo.init-ack` and MAY discard session state immediately on disconnect. Servers MUST NOT inherit `Buffer-Window` values from previous sessions during resumption; the client MUST explicitly request a buffer window on each `culpeo.init`.

### 7.4.1 Buffer-Window Limits

Implementations MUST enforce a maximum `Buffer-Window` value. The default maximum MUST NOT exceed **30,000 milliseconds** (30 seconds). Implementations MAY allow operators to configure a higher limit.

If a client requests a `Buffer-Window` exceeding the server's maximum, the server MUST clamp it to the configured maximum and reflect the actual value in `culpeo.init-ack`. The server MUST NOT allocate buffer resources based on the client's requested value before clamping.

### 7.5 Ordering and Invariants

- The client MUST send `culpeo.init` as its first frame. Any other frame before `culpeo.init-ack` is a protocol error.
- The server MUST send `culpeo.init-ack` or `culpeo.init-error` as its first frame.
- After session establishment, both parties MAY send media and event frames in any order.
- `Offset` values on media frames MUST be strictly contiguous within a stream. Each frame's offset MUST equal the next expected offset (previous offset plus the previous frame's increment). A frame with an offset that does not match the expected next offset is a protocol error and MUST result in `culpeo.close` with code `protocol-error`. Gaps and reordering are not permitted.
- Clients MUST track the highest `Offset` received per stream for use in resumption.

---

## 8. Media Streaming

### 8.1 Directionality Enforcement

The server MUST enforce stream directionality:

- `input` streams: only the client may send media frames.
- `output` streams: only the server may send media frames.
- `duplex` streams: both parties may send media frames.

A media frame sent in the wrong direction MUST cause the receiving party to close the session with `protocol-error`.

### 8.2 Offsets

Each media frame carries an `Offset` header. Offsets are per-stream, monotonically increasing integers assigned by the sender. The increment applied after each frame is determined by the stream's `offset_type` (declared in `culpeo.init`):

**`time`** — The offset increments by the number of samples per channel in the frame, providing a time-aligned cursor that is stable across reconnections regardless of frame sizing. For `audio/pcm` streams:

```
offset(n+1) = offset(n) + (frame_bytes / (channels * bits_per_sample / 8))
```

**`byte`** — The offset increments by the byte length of the raw media payload in the frame:

```
offset(n+1) = offset(n) + frame_payload_bytes
```

**`message`** — The offset increments by 1 per delivered media frame, regardless of payload size. This is the natural unit for encoded audio (Opus, AAC), JSON event payloads, or any stream where per-message count is the meaningful cursor:

```
offset(n+1) = offset(n) + 1
```

Clients MUST track the highest `Offset` received per stream and include it as `resume_offset` for that stream in any subsequent resumption `culpeo.init`.

### 8.3 Timestamps

The `Timestamp` header carries the presentation timestamp in **microseconds since session start** (since `culpeo.init-ack` was sent by the server). Timestamps are used for playout synchronization and are independent of offsets.

---

## 9. Event System

### 9.1 Protocol Events vs Application Events

CulpeoStream defines two categories of events:

**Protocol events** use the `culpeo.` namespace and are defined by this specification. Implementations MUST handle all protocol events defined here.

**Application events** are defined by upper layers and MUST use a namespaced form:

```
<namespace>.<event-name>
```

Examples: `myservice.turn-start`, `myservice.transcript`, `x-myapp.interrupt`.

Implementations that receive an event with an unknown name SHOULD ignore the frame and continue the session, ensuring forward compatibility as application layers evolve independently.

### 9.2 Stream-Scoped Events

Application events associated with a specific stream MUST include the `Stream-Id` header:

```
Event: myservice.transcript
Stream-Id: s1
Content-Type: application/json

{"text": "hello world", "is_final": true}
```

Session-scoped events not tied to a specific stream SHOULD omit `Stream-Id`.

### 9.3 Temporal Relationships

The protocol makes no assumptions about temporal relationships between application events, or between application events and media frames. Ordering guarantees, causality, and sequencing are the responsibility of the upper layer defining those events.

Applications requiring strict event ordering SHOULD use a single `duplex` stream with `content_type: application/json` for all event traffic, relying on in-order delivery guarantees of the underlying transport.

### 9.4 Event Namespace Conventions

| Prefix | Usage |
|---|---|
| `culpeo.*` | Reserved for this specification. MUST NOT be used by applications. |
| `x-<name>.*` | Recommended for private or experimental application events. |
| Any other namespaced form | Permitted for registered or well-known application protocols. |

### 9.5 Event Name Format

Event names are **case-sensitive** ASCII strings. Implementations MUST NOT normalize event names (e.g., by lowercasing or trimming whitespace). An event name MUST NOT contain leading or trailing whitespace, consecutive dots, or empty segments.

The reserved `culpeo.` prefix MUST be matched exactly — case-sensitive, no leading/trailing whitespace, no alternate encodings. Event names such as `CULPEO.init`, `culpeo..init`, or `culpeo.init ` are not valid protocol events and MUST be rejected as protocol errors.

---

## 10. Error Handling

### 10.1 Protocol Violations

If either party receives a frame that violates this specification, it MUST send `culpeo.close` with code `protocol-error` and close the connection.

### 10.2 Unknown Events

Implementations MUST first validate event name syntax per Section 9.5. A syntactically invalid event name (e.g., malformed `culpeo.*` variants) MUST be rejected as a protocol error. Only after syntax validation passes SHOULD unknown, well-formed event names be ignored per the forward-compatibility rule below.

Implementations MUST ignore frames with unknown but well-formed `Event` values and continue the session. This applies to unknown application events and future protocol events from newer versions of this specification.

### 10.3 Unknown Headers

Implementations MUST ignore unknown headers on any frame, allowing headers to be added in future versions without breaking existing implementations.

### 10.4 Connection Loss

On unexpected connection loss (no `culpeo.close` received), the client SHOULD attempt reconnection and session resumption using the last received `Offset` per stream. The client MAY implement exponential backoff between reconnection attempts.

---

## Addendum A: Authentication

Authentication is decoupled from the core protocol to allow implementors to integrate existing authorization infrastructure without modification.

### A.1 Authorization Header

Credentials are carried in the `Authorization` header using the same syntax as HTTP:

```
Authorization: <scheme> <credentials>
```

The `Bearer` scheme ([RFC 6750](https://datatracker.ietf.org/doc/html/rfc6750)) is RECOMMENDED:

```
Authorization: Bearer <token>
```

### A.2 When Authorization is Required

`Authorization` MUST be present on `culpeo.init` frames and on `culpeo.auth-response` frames. It is not required on any other frame type.

### A.3 Token Validation

Token validation is the responsibility of the server implementation. The protocol makes no assumptions about token format. Implementations MAY use JWT, opaque tokens, API keys, or any scheme compatible with the `Authorization` header syntax.

### A.4 Credential Refresh

The server MAY issue a `culpeo.auth-refresh` challenge at any time, typically when a token is approaching expiry, a security policy requires periodic re-authentication, or suspicious activity is detected.

The server MUST NOT have more than one outstanding `culpeo.auth-refresh` challenge per session at any time. The server MUST NOT issue `culpeo.auth-refresh` challenges more frequently than once per **30 seconds** per session. Implementations MAY allow operators to configure a longer minimum interval but MUST NOT allow a shorter one.

The client MUST echo the nonce from `culpeo.auth-refresh` in its `culpeo.auth-response`. If the client fails to respond within a server-defined timeout, the server MUST close the session with code `auth-expired`. Server implementations MUST enforce this timeout; the default timeout MUST NOT exceed **30 seconds**. Implementations MAY allow operators to configure a longer timeout but MUST NOT allow disabling it entirely.

Nonces MUST be single-use. After a nonce has been validated in a `culpeo.auth-response`, the server MUST discard it and MUST reject any subsequent response echoing the same nonce.

### A.5 Security Considerations

- CulpeoStream MUST be used over an encrypted transport in production.
- Tokens SHOULD have a limited lifetime.
- Nonces in `culpeo.auth-refresh` MUST be generated using a cryptographically secure random number generator and MUST be unique per challenge.
- Session IDs MUST be generated using a cryptographically secure random number generator with at least 128 bits of entropy and MUST be unguessable.
- Server implementations SHOULD rate-limit connection attempts to mitigate credential stuffing.

### A.6 Credential Confidentiality

Implementations MUST NOT include the value of the `Authorization` header in log output, error messages, exception payloads, debug traces, telemetry attributes, or any other observable channel. When diagnostic output requires reference to an authorization event, the credential value MUST be redacted or omitted entirely.

This requirement applies to all frames carrying `Authorization` — specifically `culpeo.init` and `culpeo.auth-response` — and to any internal representation of those frames (parsed structs, in-memory buffers, serialized traces).

Implementations in languages with automatic memory management SHOULD avoid retaining credential strings longer than necessary. Implementations in native languages MUST zero credential buffers after use.

---

## Addendum B: WebSocket Binding

This addendum defines how CulpeoStream frames are carried over WebSocket ([RFC 6455](https://datatracker.ietf.org/doc/html/rfc6455)).

### B.1 Sub-Protocol Declaration

Clients MUST include the `culpeostream` sub-protocol identifier in the WebSocket upgrade request:

```
Sec-WebSocket-Protocol: culpeostream
```

Servers MUST echo `Sec-WebSocket-Protocol: culpeostream` in the upgrade response. If the server does not include the sub-protocol in its response, the client MUST abort the connection before sending any CulpeoStream frames.

Servers that do not recognize the `culpeostream` sub-protocol MUST reject the upgrade request per RFC 6455 Section 4.2.2.

### B.2 Frame Type Mapping

WebSocket's native binary/text frame distinction serves as the frame type signal:

| WebSocket Frame Type | CulpeoStream Frame Type |
|---|---|
| Text frame | Control or event frame (headers + JSON body) |
| Binary frame | Media frame (headers + raw bytes) |

This eliminates the need for an explicit type field in the frame envelope and allows parsers to branch immediately on the WebSocket frame type.

### B.3 Frame Boundaries

WebSocket preserves message boundaries natively. Each WebSocket message corresponds to exactly one CulpeoStream frame. Fragmented WebSocket frames (as defined in RFC 6455 Section 5.4) SHOULD be reassembled before processing.

### B.4 Keepalive

When running over WebSocket, implementations SHOULD disable or subordinate WebSocket's native ping/pong mechanism in favor of `culpeo.ping` / `culpeo.pong`. This ensures keepalive and latency measurement behavior is consistent across all transport bindings.

### B.5 Encryption

The WebSocket binding MUST use `wss://` in production. Plain `ws://` MAY be used in local development only.

---

*CulpeoStream is an open protocol. Contributions and feedback are welcome.*
