#pragma once

// CulpeoStream Session Layer — Public API
//
// Buffer lifetime: ParsedHeadersView passed to process_*_frame() is NOT stored after the call
// returns. All needed data is copied into the session. Callers may free frame buffers immediately.
//
// Thread safety: All public methods are safe to call concurrently from separate threads.
// The session holds a single std::mutex that is acquired for the duration of each call,
// released before any transport I/O. Callbacks are invoked WITHOUT the mutex held.

#include "culpeo/message.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace culpeo::session {

// ─── Session state ────────────────────────────────────────────────────────────

enum class SessionState : uint8_t {
    uninitialized,  // Transport connected; awaiting culpeo.init
    initializing,   // culpeo.init received; validation in progress
    established,    // culpeo.init-ack sent; session active
    closed,         // Session is terminated
};

// ─── Stream types ─────────────────────────────────────────────────────────────

// Direction as declared in culpeo.init (client perspective)
enum class StreamDirection : uint8_t {
    input,   // Client → Server media
    output,  // Server → Client media
    duplex,  // Both directions
};

enum class StreamCodec : uint8_t {
    pcm,
    opus,
    aac,
    other,
};

// Offset increment behaviour for a stream, declared explicitly in culpeo.init (spec §5.5).
// REQUIRED: stream declarations without a recognised offset_type are rejected as
// invalid-streams (spec §5.6 rule 4).
enum class OffsetType : uint8_t {
    time,     // Increment by sample count per channel (PCM formula). Requires audio/pcm.
    byte,     // Increment by raw byte length of the media payload. New in spec §5.5.
    message,  // Increment by 1 per delivered frame.
};

struct PcmParams {
    uint32_t rate{};      // Samples per second
    uint16_t channels{};  // Channel count (≥ 1)
    uint16_t bits{};      // Bits per sample (positive multiple of 8)
};

// A stream declaration supplied by the client in culpeo.init
struct StreamDeclaration {
    std::string content_type;     // Raw Content-Type string (e.g. "audio/pcm;rate=16000;...")
    StreamDirection direction{};
    std::string purpose;          // Empty when not specified
    std::string hint_id;          // Client hint for resumption (ignored on fresh sessions)
    uint64_t resume_offset{0};
    bool has_resume_offset{false};
    // offset_type is REQUIRED (spec §5.6 rule 4). std::nullopt means absent from the
    // declaration JSON — validate_declarations will reject it as invalid_streams.
    std::optional<OffsetType> offset_type;
};

// A confirmed stream — after culpeo.init-ack is sent
struct StreamInfo {
    std::string id;           // Server-assigned opaque ID
    std::string content_type; // Declared content type
    StreamDirection direction{};
    std::string purpose;
    uint64_t offset{0};       // Next expected offset (send or receive)
    StreamCodec codec{StreamCodec::other};
    std::optional<PcmParams> pcm_params;
    OffsetType offset_type{OffsetType::message};  // Explicit offset increment behaviour
};

// ─── Session resumption ───────────────────────────────────────────────────────

// Exported by Session::export_state() for application-layer persistence.
// Pass as prior_state when constructing the new Session for a reconnecting client.
struct PersistedSessionState {
    std::string session_id;
    std::vector<StreamInfo> streams;
    uint32_t buffer_window_ms{0};
    std::chrono::steady_clock::time_point disconnected_at{};
};

// ─── Error codes ─────────────────────────────────────────────────────────────

enum class Error : uint8_t {
    wrong_state,            // Frame received in the wrong session state
    protocol_error,         // Generic protocol violation
    stream_not_found,       // Unknown Stream-Id on media frame
    invalid_direction,      // Media sent in a direction inconsistent with stream type
    version_unsupported,    // Protocol version not supported
    invalid_streams,        // Stream declarations violate Section 5.5
    offset_mismatch,        // Offset header doesn't equal next expected offset
    offset_overflow,        // Integer overflow in PCM offset increment computation
    auth_failed,            // Bearer token validation failed
    nonce_mismatch,         // Echoed nonce doesn't match issued nonce
    nonce_already_pending,  // Auth-refresh issued while one is already outstanding
    ping_rate_exceeded,     // Ping dropped due to rate limit (not a hard error)
    json_error,             // JSON body could not be parsed
    transport_error,        // Transport send threw or failed
    content_type_mismatch,  // Media frame content-type doesn't match stream declaration
    session_expired,        // Session-Id not found or too old for resumption
    max_streams_exceeded,   // culpeo.init declares more streams than configured max
    nonce_expired,          // Outstanding nonce timed out before response
    invalid_event_name,     // Event name violates Section 9.5 syntax rules
};

[[nodiscard]] std::string_view error_message(Error error) noexcept;

// ─── Transport abstraction ────────────────────────────────────────────────────

class ITransport {
public:
    virtual ~ITransport() = default;

    // Send a text (control/event) frame. Called without the session mutex held.
    virtual void send_text(std::span<const std::byte> frame) = 0;

    // Send a binary (media) frame. Called without the session mutex held.
    virtual void send_binary(std::span<const std::byte> frame) = 0;

    // Close the underlying connection.
    virtual void close() = 0;
};

// ─── Session configuration ────────────────────────────────────────────────────

struct SessionConfig {
    // Maximum buffer window the server will negotiate (ms). Default and spec max: 30 000.
    uint32_t max_buffer_window_ms{30'000};

    // Used when client omits Buffer-Window in culpeo.init.
    uint32_t default_buffer_window_ms{5'000};

    // Max streams per session. Spec default max: 16.
    uint32_t max_streams{16};

    // Max pings per second per session. Spec default max: 5.
    uint32_t max_ping_rate_per_second{5};

    // Auth-response timeout in seconds. Spec default max: 30.
    uint32_t auth_refresh_timeout_s{30};

    // Minimum interval between auth-refresh challenges (spec minimum: 30 s).
    uint32_t min_auth_refresh_interval_s{30};

    // Protocol versions this server supports. Must include at least "0.3".
    std::vector<std::string> supported_versions{"0.3"};
};

// ─── Callbacks ───────────────────────────────────────────────────────────────

// Called to validate a Bearer token from culpeo.init. Return true if valid.
// IMPORTANT: Do NOT log or store the token value; it is sensitive (spec §A.6).
using AuthValidateCallback = std::function<bool(std::string_view bearer_token)>;

// Called when a media frame is received from the client on an input/duplex stream.
// The span is only valid for the duration of the callback.
using MediaReceivedCallback =
    std::function<void(const StreamInfo&, uint64_t timestamp_us, std::span<const std::byte> body)>;

// Called after a pong is received in response to a server-initiated culpeo.ping.
using RttCallback = std::function<void(std::chrono::microseconds rtt)>;

// Called when the session closes (either side). code is a machine-readable close code.
using CloseCallback = std::function<void(std::string_view code, std::string_view reason)>;

// Called when culpeo.auth-response is received with a new token.
// Return true if the new token is valid.
using AuthResponseCallback = std::function<bool(std::string_view bearer_token)>;

struct SessionCallbacks {
    AuthValidateCallback on_auth_validate;      // Required for auth enforcement
    MediaReceivedCallback on_media_received;    // Optional: receive media frames
    RttCallback on_rtt;                         // Optional: RTT measurement
    CloseCallback on_close;                     // Optional: close notification
    AuthResponseCallback on_auth_response;      // Required for auth-refresh flow
};

// ─── Session ─────────────────────────────────────────────────────────────────

class Session {
public:
    // Construct a fresh session (no prior state — server-side for new connections).
    explicit Session(
        ITransport& transport,
        SessionCallbacks callbacks = {},
        SessionConfig config = {},
        std::optional<PersistedSessionState> prior_state = std::nullopt);

    ~Session();

    // Non-copyable and non-movable (owns a mutex).
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    // ── Inbound frame processing ──────────────────────────────────────────────

    // Feed a parsed control (text) frame. Thread-safe.
    [[nodiscard]] std::expected<void, Error>
    process_control_frame(const culpeo::message::ParsedHeadersView& frame) noexcept;

    // Feed a parsed media (binary) frame. Thread-safe.
    [[nodiscard]] std::expected<void, Error>
    process_media_frame(const culpeo::message::ParsedHeadersView& frame) noexcept;

    // ── Server-initiated sends ────────────────────────────────────────────────

    // Send a media payload on an output/duplex stream. Thread-safe.
    [[nodiscard]] std::expected<void, Error>
    send_media(std::string_view stream_id,
               std::span<const std::byte> payload,
               uint64_t timestamp_us) noexcept;

    // Issue a culpeo.auth-refresh challenge. Thread-safe.
    [[nodiscard]] std::expected<void, Error> send_auth_refresh() noexcept;

    // Send a culpeo.ping to measure round-trip time. Thread-safe.
    [[nodiscard]] std::expected<void, Error> send_ping() noexcept;

    // Initiate graceful close. Thread-safe.
    void close(std::string_view code = "normal", std::string_view reason = "Closing") noexcept;

    // ── Inspection ───────────────────────────────────────────────────────────

    [[nodiscard]] SessionState state() const noexcept;
    [[nodiscard]] std::optional<std::string> session_id() const noexcept;
    [[nodiscard]] std::vector<StreamInfo> streams() const noexcept;

    // Export state for application-layer persistence (call before destruction
    // when you want to support resumption by a reconnecting client).
    [[nodiscard]] std::optional<PersistedSessionState> export_state() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace culpeo::session
