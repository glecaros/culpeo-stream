#include "culpeo/session.hpp"

#include "auth_refresh.hpp"
#include "crypto.hpp"
#include "offset_tracker.hpp"
#include "stream_registry.hpp"

#include "culpeo/message.hpp"

#include <charconv>
#include <chrono>
#include <cstring>
#include <format>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

// nlohmann/json for body parsing
#include <nlohmann/json.hpp>

namespace culpeo::session {

using namespace internal;

// ─── error_message ────────────────────────────────────────────────────────────

[[nodiscard]] std::string_view error_message(Error error) noexcept {
    switch (error) {
    case Error::wrong_state:           return "frame received in wrong session state";
    case Error::protocol_error:        return "protocol violation";
    case Error::stream_not_found:      return "unknown Stream-Id";
    case Error::invalid_direction:     return "media sent in direction inconsistent with stream type";
    case Error::version_unsupported:   return "protocol version not supported";
    case Error::invalid_streams:       return "stream declarations violate spec §5.5";
    case Error::offset_mismatch:       return "offset does not match expected next value";
    case Error::offset_overflow:       return "integer overflow in offset computation";
    case Error::auth_failed:           return "bearer token validation failed";
    case Error::nonce_mismatch:        return "echoed nonce does not match issued nonce";
    case Error::nonce_already_pending: return "auth-refresh already outstanding";
    case Error::ping_rate_exceeded:    return "ping rate limit exceeded (dropped)";
    case Error::json_error:            return "JSON body could not be parsed";
    case Error::transport_error:       return "transport send failed";
    case Error::content_type_mismatch: return "media frame content-type does not match stream";
    case Error::session_expired:       return "session-id not found or expired";
    case Error::max_streams_exceeded:  return "stream count exceeds server maximum";
    case Error::nonce_expired:         return "auth-refresh nonce timed out";
    case Error::invalid_event_name:    return "event name violates §9.5 syntax rules";
    }
    return "unknown error";
}

// ─── JSON helpers (internal only) ─────────────────────────────────────────────

namespace {

// Serialize the init-ack / init-error body JSON.
// is_resumption: include resume_offset in each stream entry.
std::string build_init_ack_body(std::string_view version,
                                 const std::vector<StreamInfo>& streams,
                                 bool is_resumption) {
    nlohmann::json body;
    body["version"] = version;
    auto& sarr = body["streams"];
    sarr = nlohmann::json::array();

    for (const auto& s : streams) {
        nlohmann::json entry;
        entry["id"] = s.id;
        entry["content_type"] = s.content_type;
        entry["type"] = stream_direction_to_string(s.direction);
        entry["offset_type"] = offset_type_to_string(s.offset_type);
        if (!s.purpose.empty()) {
            entry["purpose"] = s.purpose;
        }
        if (is_resumption) {
            entry["resume_offset"] = s.offset;
        }
        sarr.push_back(std::move(entry));
    }

    return body.dump();
}

std::string build_init_error_body(const std::vector<std::string>& supported_versions) {
    nlohmann::json body;
    body["supported_versions"] = supported_versions;
    return body.dump();
}

std::string build_pong_body(int64_t original_ts, int64_t server_ts) {
    // Avoid nlohmann for a simple two-field object (performance)
    std::string result = R"({"ts":)";
    result += std::to_string(original_ts);
    result += R"(,"server_ts":)";
    result += std::to_string(server_ts);
    result += '}';
    return result;
}

std::string build_auth_refresh_body(std::string_view nonce_hex) {
    nlohmann::json body;
    body["nonce"] = nonce_hex;
    return body.dump();
}

std::string build_close_body() { return "{}"; }

// Parse a uint64 from a string_view (for Offset and Timestamp headers).
std::expected<uint64_t, Error> parse_uint64(std::string_view sv) noexcept {
    if (sv.empty()) return std::unexpected(Error::protocol_error);

    // Reject leading zeros per spec §4.2 (no leading zeros on integer headers)
    if (sv.size() > 1 && sv[0] == '0') return std::unexpected(Error::protocol_error);

    uint64_t result = 0;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), result);
    if (ec != std::errc{} || ptr != sv.data() + sv.size()) {
        return std::unexpected(Error::protocol_error);
    }
    return result;
}

// Validate event name syntax (spec §9.5).
// Must not contain leading/trailing whitespace, consecutive dots, or empty segments.
bool is_valid_event_name(std::string_view name) noexcept {
    if (name.empty()) return false;
    if (name.front() == ' ' || name.front() == '\t' ||
        name.back()  == ' ' || name.back()  == '\t') return false;

    // Must contain at least one dot (namespace separator)
    if (name.find('.') == std::string_view::npos) return false;

    // No consecutive dots
    for (std::size_t i = 1; i < name.size(); ++i) {
        if (name[i] == '.' && name[i - 1] == '.') return false;
    }

    // No leading or trailing dot
    if (name.front() == '.' || name.back() == '.') return false;

    return true;
}

// Serialize a frame to bytes (uses culpeo::message helpers).
std::vector<std::byte> make_text_frame(
    std::initializer_list<culpeo::message::HeaderFieldView> headers,
    std::string_view body) {
    auto result = culpeo::message::serialize_frame(
        culpeo::message::FrameType::control,
        std::span<const culpeo::message::HeaderFieldView>(headers.begin(), headers.size()),
        culpeo::message::as_bytes(body));
    if (!result) return {};
    return std::move(*result);
}

// Send a text frame; catches any transport exceptions.
// Caller must NOT hold the session mutex when calling this.
void send_text_frame_noexcept(ITransport& transport,
                               std::initializer_list<culpeo::message::HeaderFieldView> headers,
                               std::string_view body) noexcept {
    try {
        auto frame = make_text_frame(headers, body);
        if (!frame.empty()) {
            transport.send_text(frame);
        }
    } catch (...) {
        // Transport errors during send are non-fatal from the session's perspective
    }
}

// Returns current time in microseconds since Unix epoch.
int64_t now_us() noexcept {
    const auto tp = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(tp).count());
}

// ─── Content-Type comparison (spec §6.2) ─────────────────────────────────────
// type/subtype and parameter names are compared case-insensitively;
// parameter values are compared case-sensitively.
// Parameter order is insignificant.

[[nodiscard]] constexpr char to_lower_ascii_ct(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] static bool iequals_sv(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (to_lower_ascii_ct(a[i]) != to_lower_ascii_ct(b[i])) return false;
    }
    return true;
}

static std::string_view trim_sv(std::string_view sv) noexcept {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) sv.remove_prefix(1);
    while (!sv.empty() && (sv.back()  == ' ' || sv.back()  == '\t')) sv.remove_suffix(1);
    return sv;
}

// Parse a Content-Type string into {type/subtype, [{name, value}...]} without allocation.
// Returns true on success; false on malformed input.
struct CtParam { std::string_view name; std::string_view value; };

static bool content_type_matches(std::string_view declared, std::string_view received) noexcept {
    // Helper: parse "type/subtype[;param=value...]" into type/subtype and param list.
    // Returns false on parse error.
    const auto parse = [](std::string_view ct,
                           std::string_view& type_subtype,
                           std::array<CtParam, 16>& params,
                           std::size_t& param_count) -> bool {
        const auto semi = ct.find(';');
        type_subtype = trim_sv(ct.substr(0, semi));
        param_count = 0;
        if (semi == std::string_view::npos) return true;

        std::size_t cursor = semi + 1;
        while (cursor < ct.size()) {
            const auto next = ct.find(';', cursor);
            const auto seg = trim_sv(ct.substr(cursor, next == std::string_view::npos
                                                        ? ct.size() - cursor
                                                        : next - cursor));
            cursor = (next == std::string_view::npos) ? ct.size() : next + 1;
            if (seg.empty()) return false;
            const auto eq = seg.find('=');
            if (eq == std::string_view::npos) return false;
            if (param_count >= 16) return false;
            params[param_count++] = {trim_sv(seg.substr(0, eq)),
                                     trim_sv(seg.substr(eq + 1))};
        }
        return true;
    };

    std::string_view d_ts, r_ts;
    std::array<CtParam, 16> d_params{}, r_params{};
    std::size_t d_count = 0, r_count = 0;

    if (!parse(declared, d_ts, d_params, d_count)) return false;
    if (!parse(received, r_ts, r_params, r_count)) return false;

    // Compare type/subtype case-insensitively
    if (!iequals_sv(d_ts, r_ts)) return false;

    // Both must have the same number of params
    if (d_count != r_count) return false;

    // For each param in declared, find a matching param in received:
    //   name case-insensitive, value case-sensitive.
    for (std::size_t i = 0; i < d_count; ++i) {
        bool found = false;
        for (std::size_t j = 0; j < r_count; ++j) {
            if (iequals_sv(d_params[i].name, r_params[j].name) &&
                d_params[i].value == r_params[j].value) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

// ─── Cryptographic helpers ────────────────────────────────────────────────────

// Generate a cryptographically random 64-bit nonce via the OS CSPRNG.
// Returns 0 only on failure (extremely rare); caller should treat 0 as error
// or re-try, but in practice the CSPRNG never returns 0 for all 8 bytes.
[[nodiscard]] static uint64_t generate_u64_nonce() noexcept {
    uint64_t nonce = 0;
    try {
        culpeo::crypto::secure_random(
            std::span<std::byte>(reinterpret_cast<std::byte*>(&nonce), sizeof(nonce)));
    } catch (...) {
        return 0;
    }
    // Avoid 0 (used as "no nonce") — re-roll if needed (probability 1/2^64)
    if (nonce == 0) nonce = 1;
    return nonce;
}

}  // namespace

// ─── Session::Impl ────────────────────────────────────────────────────────────

struct Session::Impl {
    ITransport& transport;
    SessionCallbacks callbacks;
    SessionConfig config;
    std::optional<PersistedSessionState> prior_state;  // For resumption

    mutable std::mutex mutex;

    // ── State machine ─────────────────────────────────────────────────────────
    SessionState state{SessionState::uninitialized};
    std::string session_id_str;
    std::string negotiated_version;
    uint32_t negotiated_buffer_window_ms{0};
    bool is_resumed_session{false};

    // ── Stream registry ───────────────────────────────────────────────────────
    StreamRegistry registry;

    // ── Auth refresh ──────────────────────────────────────────────────────────
    AuthRefreshManager auth_mgr;

    // ── Ping rate limiting ────────────────────────────────────────────────────
    std::chrono::steady_clock::time_point ping_window_start{};
    uint32_t pings_in_window{0};

    // ── Ping RTT tracking (for server-initiated pings) ────────────────────────
    std::optional<uint64_t> pending_ping_nonce;   // Cryptographically random nonce
    std::optional<int64_t>  pending_ping_send_ts; // Microsecond timestamp at send time (for RTT)

    // ── Constructor ───────────────────────────────────────────────────────────
    explicit Impl(ITransport& t, SessionCallbacks cb, SessionConfig cfg,
                  std::optional<PersistedSessionState> ps)
        : transport(t)
        , callbacks(std::move(cb))
        , config(std::move(cfg))
        , prior_state(std::move(ps))
        , registry(config.max_streams) {}

    // ─ Internal helpers (called with mutex held) ──────────────────────────────

    void transition_to_closed_locked() noexcept {
        state = SessionState::closed;
        auth_mgr.clear();
        // NOTE: registry is kept so export_state() still works after close
    }

    // Send init-ack then transition to established. Mutex must be held by caller.
    // Transport send is done AFTER releasing the mutex.
    [[nodiscard]] std::expected<void, Error>
    finalize_established_locked(const std::string& sid,
                                 const std::string& version,
                                 uint32_t buffer_window_ms,
                                 bool resumption,
                                 std::unique_lock<std::mutex>& lock) noexcept {
        session_id_str = sid;
        negotiated_version = version;
        negotiated_buffer_window_ms = buffer_window_ms;
        is_resumed_session = resumption;
        state = SessionState::established;

        auto snapshot = registry.snapshot();
        std::string bw_str = std::to_string(buffer_window_ms);
        std::string body = build_init_ack_body(version, snapshot, resumption);

        // Release mutex before transport I/O to avoid holding lock during send
        lock.unlock();

        send_text_frame_noexcept(transport,
            {{"Event", "culpeo.init-ack"},
             {"Session-Id", sid},
             {"Buffer-Window", bw_str},
             {"Content-Type", "application/json"}},
            body);

        return {};
    }

    // Map a CulpeoStream protocol close code to the appropriate WebSocket status code.
    // RFC 6455 §7.4.1 close codes.
    static int to_ws_close_code(std::string_view culpeo_code) noexcept {
        if (culpeo_code == "normal")                       return 1000;
        if (culpeo_code == "unauthorized"   ||
            culpeo_code == "auth-expired"   ||
            culpeo_code == "auth-failed")                  return 1008;
        // RFC 6455 §7.4.1: 1001 Going Away — server is shutting down or
        // the endpoint is going away (idle timeout fits this semantic).
        if (culpeo_code == "server-shutdown" ||
            culpeo_code == "idle-timeout")                 return 1001;
        // All other codes ("protocol-error", "version-unsupported",
        // "invalid-streams", "session-expired", etc.) → 1002 Protocol Error.
        return 1002;
    }

    // Close the transport with a WebSocket close code derived from a
    // CulpeoStream protocol code.  Swallows all exceptions (close is best-effort).
    void close_transport(std::string_view culpeo_code, std::string_view reason) noexcept {
        try { transport.close(to_ws_close_code(culpeo_code), reason); } catch (...) {}
    }

    // Send init-error frame and close. Mutex must NOT be held.
    void send_init_error_and_close(std::string_view code,
                                    std::string_view reason,
                                    std::string_view body) noexcept {
        send_text_frame_noexcept(transport,
            {{"Event", "culpeo.init-error"},
             {"Code", code},
             {"Reason", reason},
             {"Content-Type", "application/json"}},
            body);
        close_transport(code, reason);
    }

    // Send culpeo.close and transition. Caller must have released the mutex.
    void send_close_frame(std::string_view code, std::string_view reason) noexcept {
        send_text_frame_noexcept(transport,
            {{"Event", "culpeo.close"},
             {"Code", code},
             {"Reason", reason},
             {"Content-Type", "application/json"}},
            build_close_body());
    }

    // ─ Event handlers (called with mutex held, but release before I/O) ────────

    [[nodiscard]] std::expected<void, Error>
    handle_init(const culpeo::message::ParsedHeadersView& f,
                 std::unique_lock<std::mutex>& lock) noexcept {
        if (state != SessionState::uninitialized) {
            // A second culpeo.init after session is established is a protocol error
            state = SessionState::initializing;  // Intermediate before sending error
            lock.unlock();
            send_init_error_and_close("protocol-error",
                "culpeo.init received after session established", "{}");
            std::unique_lock<std::mutex> re(mutex);
            transition_to_closed_locked();
            return std::unexpected(Error::protocol_error);
        }

        state = SessionState::initializing;

        // ── Parse body ────────────────────────────────────────────────────────
        nlohmann::json body_json;
        try {
            body_json = nlohmann::json::parse(f.body);
        } catch (...) {
            lock.unlock();
            send_init_error_and_close("protocol-error", "Invalid JSON body", "{}");
            std::unique_lock<std::mutex> re(mutex);
            transition_to_closed_locked();
            return std::unexpected(Error::json_error);
        }

        // ── Version negotiation (spec §7.3) ───────────────────────────────────
        std::string client_version;
        try {
            client_version = body_json.at("version").get<std::string>();
        } catch (...) {
            lock.unlock();
            send_init_error_and_close("protocol-error", "Missing version field", "{}");
            std::unique_lock<std::mutex> re(mutex);
            transition_to_closed_locked();
            return std::unexpected(Error::json_error);
        }

        bool version_ok = false;
        for (const auto& v : config.supported_versions) {
            if (v == client_version) { version_ok = true; break; }
        }
        if (!version_ok) {
            lock.unlock();
            send_init_error_and_close("unsupported-version",
                "Protocol version not supported",
                build_init_error_body(config.supported_versions));
            std::unique_lock<std::mutex> re(mutex);
            transition_to_closed_locked();
            return std::unexpected(Error::version_unsupported);
        }

        // ── Auth validation ───────────────────────────────────────────────────
        // Bearer token must NOT be logged (spec §A.6)
        // Unlock before invoking the callback to avoid deadlock if the
        // callback re-enters the session (same pattern as on_auth_response).
        bool auth_ok = true;
        if (callbacks.on_auth_validate) {
            std::string bearer_str; // copy so it is valid after lock release
            if (f.authorization.has_value()) {
                const auto& auth_val = *f.authorization;
                constexpr std::string_view bearer_prefix = "Bearer ";
                if (auth_val.starts_with(bearer_prefix)) {
                    bearer_str = std::string(auth_val.substr(bearer_prefix.size()));
                }
            }
            auto cb = callbacks.on_auth_validate;
            lock.unlock();
            auth_ok = cb(bearer_str);
            // Relock after callback returns; re-check state in case close raced.
            lock = std::unique_lock<std::mutex>(mutex);
            if (state == SessionState::closed) {
                return std::unexpected(Error::auth_failed);
            }
        }

        if (!auth_ok) {
            lock.unlock();
            send_init_error_and_close("unauthorized", "Credentials invalid", "{}");
            std::unique_lock<std::mutex> re(mutex);
            transition_to_closed_locked();
            return std::unexpected(Error::auth_failed);
        }

        // ── Parse stream declarations ─────────────────────────────────────────
        std::vector<StreamDeclaration> decls;
        try {
            const auto& streams_arr = body_json.at("streams");
            for (const auto& s : streams_arr) {
                StreamDeclaration d{};
                d.content_type = s.at("content_type").get<std::string>();

                const std::string type_str = s.at("type").get<std::string>();
                const auto dir = parse_stream_direction(type_str);
                if (!dir) {
                    lock.unlock();
                    send_init_error_and_close("invalid-streams",
                        "Invalid stream type", "{}");
                    std::unique_lock<std::mutex> re(mutex);
                    transition_to_closed_locked();
                    return std::unexpected(Error::invalid_streams);
                }
                d.direction = *dir;

                if (s.contains("purpose")) {
                    d.purpose = s["purpose"].get<std::string>();
                }
                if (s.contains("id")) {
                    d.hint_id = s["id"].get<std::string>();
                }
                if (s.contains("resume_offset")) {
                    d.resume_offset = s["resume_offset"].get<uint64_t>();
                    d.has_resume_offset = true;
                }
                // offset_type is REQUIRED (spec §5.6). Parse and reject unknown values.
                if (!s.contains("offset_type")) {
                    lock.unlock();
                    send_init_error_and_close("invalid-streams",
                        "Stream missing required offset_type field", "{}");
                    std::unique_lock<std::mutex> re(mutex);
                    transition_to_closed_locked();
                    return std::unexpected(Error::invalid_streams);
                }
                {
                    const std::string ot_str = s["offset_type"].get<std::string>();
                    const auto ot = parse_offset_type(ot_str);
                    if (!ot) {
                        lock.unlock();
                        send_init_error_and_close("invalid-streams",
                            "Unrecognised offset_type value", "{}");
                        std::unique_lock<std::mutex> re(mutex);
                        transition_to_closed_locked();
                        return std::unexpected(Error::invalid_streams);
                    }
                    d.offset_type = *ot;
                }
                decls.push_back(std::move(d));
            }
        } catch (...) {
            lock.unlock();
            send_init_error_and_close("protocol-error", "Invalid streams JSON", "{}");
            std::unique_lock<std::mutex> re(mutex);
            transition_to_closed_locked();
            return std::unexpected(Error::json_error);
        }

        // ── Resumption path ───────────────────────────────────────────────────
        if (f.session_id.has_value()) {
            return handle_resumption_locked(f, decls, client_version, lock);
        }

        // ── Fresh session path ────────────────────────────────────────────────
        // Validate declarations
        auto val = validate_declarations(decls, config.max_streams);
        if (!val) {
            const bool too_many = (val.error() == Error::max_streams_exceeded);
            lock.unlock();
            send_init_error_and_close("invalid-streams",
                too_many ? "Too many streams" : "Stream declarations invalid", "{}");
            std::unique_lock<std::mutex> re(mutex);
            transition_to_closed_locked();
            return std::unexpected(val.error());
        }

        // Register streams
        auto reg_result = registry.register_from_declarations(decls);
        if (!reg_result) {
            lock.unlock();
            send_init_error_and_close("server-error", "Failed to allocate streams", "{}");
            std::unique_lock<std::mutex> re(mutex);
            transition_to_closed_locked();
            return std::unexpected(reg_result.error());
        }

        // Generate session ID (128 bits = 16 bytes → 32 hex chars)
        auto sid_result = generate_csprng_id(16);
        if (!sid_result) {
            lock.unlock();
            send_init_error_and_close("server-error", "Failed to generate session ID", "{}");
            std::unique_lock<std::mutex> re(mutex);
            transition_to_closed_locked();
            return std::unexpected(sid_result.error());
        }

        // Negotiate buffer window
        uint32_t bw = config.default_buffer_window_ms;
        if (f.buffer_window.has_value()) {
            auto bw_parsed = parse_uint64(*f.buffer_window);
            if (bw_parsed) {
                bw = static_cast<uint32_t>(
                    std::min<uint64_t>(*bw_parsed, config.max_buffer_window_ms));
            }
        }

        return finalize_established_locked(*sid_result, client_version, bw, false, lock);
    }

    [[nodiscard]] std::expected<void, Error>
    handle_resumption_locked(const culpeo::message::ParsedHeadersView& f,
                              const std::vector<StreamDeclaration>& decls,
                              const std::string& version,
                              std::unique_lock<std::mutex>& lock) noexcept {
        if (!prior_state.has_value()) {
            lock.unlock();
            send_init_error_and_close("invalid-session", "Session not found", "{}");
            std::unique_lock<std::mutex> re(mutex);
            transition_to_closed_locked();
            return std::unexpected(Error::session_expired);
        }

        const auto& ps = *prior_state;

        // Check session ID matches
        if (std::string(*f.session_id) != ps.session_id) {
            lock.unlock();
            send_init_error_and_close("invalid-session", "Session ID mismatch", "{}");
            std::unique_lock<std::mutex> re(mutex);
            transition_to_closed_locked();
            return std::unexpected(Error::session_expired);
        }

        // Check if session has expired (wall-clock since disconnection)
        if (ps.buffer_window_ms > 0) {
            const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - ps.disconnected_at).count();
            if (elapsed_ms > static_cast<long long>(ps.buffer_window_ms)) {
                lock.unlock();
                send_init_error_and_close("invalid-session", "Session expired", "{}");
                std::unique_lock<std::mutex> re(mutex);
                transition_to_closed_locked();
                return std::unexpected(Error::session_expired);
            }
        }

        // Validate that declared streams match the original session
        // Matching by type, content_type, purpose (spec §5.3)
        if (decls.size() != ps.streams.size()) {
            lock.unlock();
            send_init_error_and_close("invalid-streams",
                "Stream set does not match original session", "{}");
            std::unique_lock<std::mutex> re(mutex);
            transition_to_closed_locked();
            return std::unexpected(Error::invalid_streams);
        }

        // Restore streams from persisted state; adjust offsets per resume request
        std::vector<StreamInfo> restored = ps.streams;

        for (auto& restored_stream : restored) {
            // Find matching declaration
            const StreamDeclaration* matching_decl = nullptr;
            for (const auto& d : decls) {
                if (d.direction == restored_stream.direction &&
                    d.content_type == restored_stream.content_type &&
                    d.purpose == restored_stream.purpose) {
                    matching_decl = &d;
                    break;
                }
            }
            if (!matching_decl) {
                lock.unlock();
                send_init_error_and_close("invalid-streams",
                    "Cannot match all streams in resumption", "{}");
                std::unique_lock<std::mutex> re(mutex);
                transition_to_closed_locked();
                return std::unexpected(Error::invalid_streams);
            }

            if (matching_decl->has_resume_offset) {
                const uint64_t req = matching_decl->resume_offset;
                // If requested offset exceeds server's current → reject (spec §7.2)
                if (req > restored_stream.offset) {
                    lock.unlock();
                    send_init_error_and_close("invalid-streams",
                        "resume_offset exceeds server offset", "{}");
                    std::unique_lock<std::mutex> re(mutex);
                    transition_to_closed_locked();
                    return std::unexpected(Error::invalid_streams);
                }
                // Clamp confirmed offset: max(requested, earliest available)
                // For simplicity we treat requested as the confirmed offset
                // (no partial eviction in our reference implementation)
                restored_stream.offset = req;
            }
        }

        registry.register_from_persisted(restored);

        // Negotiate buffer window
        uint32_t bw = config.default_buffer_window_ms;
        if (f.buffer_window.has_value()) {
            auto bw_parsed = parse_uint64(*f.buffer_window);
            if (bw_parsed) {
                bw = static_cast<uint32_t>(
                    std::min<uint64_t>(*bw_parsed, config.max_buffer_window_ms));
            }
        }

        return finalize_established_locked(ps.session_id, version, bw, true, lock);
    }

    [[nodiscard]] std::expected<void, Error>
    handle_ping(const culpeo::message::ParsedHeadersView& f,
                 std::unique_lock<std::mutex>& lock) noexcept {
        if (state != SessionState::established) {
            return std::unexpected(Error::wrong_state);
        }

        // Rate limiting: 5 pings per second (spec §6.1)
        const auto now = std::chrono::steady_clock::now();
        const auto window_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - ping_window_start).count();

        if (window_elapsed >= 1000) {
            // Start a new 1-second window
            ping_window_start = now;
            pings_in_window = 1;
        } else {
            ++pings_in_window;
            if (pings_in_window > config.max_ping_rate_per_second) {
                // Silently drop — spec says SHOULD not close session
                return std::unexpected(Error::ping_rate_exceeded);
            }
        }

        // Parse ts from body
        int64_t ts = 0;
        try {
            auto body_json = nlohmann::json::parse(f.body);
            ts = body_json.at("ts").get<int64_t>();
        } catch (...) {
            // Malformed ping body — send a pong with ts=0 to avoid breaking keepalive
            ts = 0;
        }

        const int64_t server_ts = now_us();
        std::string pong_body = build_pong_body(ts, server_ts);

        lock.unlock();

        send_text_frame_noexcept(transport,
            {{"Event", "culpeo.pong"},
             {"Content-Type", "application/json"}},
            pong_body);

        return {};
    }

    [[nodiscard]] std::expected<void, Error>
    handle_pong(const culpeo::message::ParsedHeadersView& f,
                 std::unique_lock<std::mutex>& lock) noexcept {
        if (state != SessionState::established) {
            return std::unexpected(Error::wrong_state);
        }

        if (!pending_ping_nonce.has_value()) {
            // Unsolicited pong — ignore
            return {};
        }

        uint64_t echoed_nonce = 0;
        try {
            auto body_json = nlohmann::json::parse(f.body);
            echoed_nonce = body_json.at("nonce").get<uint64_t>();
        } catch (...) {
            pending_ping_nonce.reset();
            pending_ping_send_ts.reset();
            return {};
        }

        if (echoed_nonce == *pending_ping_nonce) {
            const int64_t now = now_us();
            const auto rtt = std::chrono::microseconds(
                now - pending_ping_send_ts.value_or(now));
            pending_ping_nonce.reset();
            pending_ping_send_ts.reset();

            auto cb = callbacks.on_rtt;
            lock.unlock();
            if (cb) cb(rtt);
        } else {
            pending_ping_nonce.reset();
            pending_ping_send_ts.reset();
        }

        return {};
    }

    [[nodiscard]] std::expected<void, Error>
    handle_auth_response(const culpeo::message::ParsedHeadersView& f,
                          std::unique_lock<std::mutex>& lock) noexcept {
        if (state != SessionState::established) {
            return std::unexpected(Error::wrong_state);
        }

        if (!auth_mgr.is_pending()) {
            // No outstanding challenge — protocol error
            lock.unlock();
            send_close_frame("protocol-error", "Unexpected culpeo.auth-response");
            close_transport("protocol-error", "Unexpected culpeo.auth-response");
            std::unique_lock<std::mutex> re(mutex);
            transition_to_closed_locked();
            return std::unexpected(Error::protocol_error);
        }

        // Parse nonce from body
        std::string echoed_nonce;
        try {
            auto body_json = nlohmann::json::parse(f.body);
            echoed_nonce = body_json.at("nonce").get<std::string>();
        } catch (...) {
            // Bad JSON → treat as invalid nonce
            auth_mgr.clear();
            lock.unlock();
            send_close_frame("unauthorized", "Invalid auth-response body");
            close_transport("unauthorized", "Invalid auth-response body");
            std::unique_lock<std::mutex> re(mutex);
            transition_to_closed_locked();
            return std::unexpected(Error::json_error);
        }

        // Validate nonce (constant-time, zeroes buffer on return)
        auto nonce_result = auth_mgr.validate_and_consume(echoed_nonce, config.auth_refresh_timeout_s);

        // Zero the echoed_nonce string from memory
        std::fill(echoed_nonce.begin(), echoed_nonce.end(), '\0');

        if (!nonce_result) {
            const bool expired = (nonce_result.error() == Error::nonce_expired);
            lock.unlock();
            send_close_frame("auth-expired", expired ? "Auth-refresh nonce expired"
                                                     : "Nonce mismatch");
            close_transport("auth-expired", expired ? "Auth-refresh nonce expired" : "Nonce mismatch");
            std::unique_lock<std::mutex> re(mutex);
            transition_to_closed_locked();
            return std::unexpected(nonce_result.error());
        }

        // Validate the new token
        bool token_ok = true;
        if (callbacks.on_auth_response) {
            std::string_view bearer;
            if (f.authorization.has_value()) {
                const auto& auth_val = *f.authorization;
                constexpr std::string_view pfx = "Bearer ";
                if (auth_val.starts_with(pfx)) bearer = auth_val.substr(pfx.size());
            }
            auto cb = callbacks.on_auth_response;
            lock.unlock();
            token_ok = cb(bearer);
        } else {
            lock.unlock();
        }

        if (!token_ok) {
            std::unique_lock<std::mutex> re(mutex);
            if (state != SessionState::closed) {
                transition_to_closed_locked();
                re.unlock();
                send_close_frame("auth-expired", "New credentials rejected");
                close_transport("auth-expired", "New credentials rejected");
            }
            return std::unexpected(Error::auth_failed);
        }

        return {};
    }

    [[nodiscard]] std::expected<void, Error>
    handle_close(const culpeo::message::ParsedHeadersView& f,
                  std::unique_lock<std::mutex>& lock) noexcept {
        if (state == SessionState::closed) {
            return {};  // Already closed, ignore
        }

        std::string_view code = f.code.value_or("normal");
        std::string_view reason = f.reason.value_or("Connection closed");

        auth_mgr.clear();
        transition_to_closed_locked();

        auto cb = callbacks.on_close;
        std::string code_str(code);
        std::string reason_str(reason);
        lock.unlock();

        // Respond with our own close (spec §6.1: "SHOULD respond with its own culpeo.close")
        send_close_frame(code_str, reason_str);
        close_transport(code_str, reason_str);

        if (cb) cb(code_str, reason_str);

        return {};
    }
};

// ─── Session public methods ────────────────────────────────────────────────────

Session::Session(ITransport& transport,
                 SessionCallbacks callbacks,
                 SessionConfig config,
                 std::optional<PersistedSessionState> prior_state)
    : impl_(std::make_unique<Impl>(transport, std::move(callbacks),
                                    std::move(config), std::move(prior_state))) {}

Session::~Session() = default;

// ─── process_control_frame ────────────────────────────────────────────────────

std::expected<void, Error>
Session::process_control_frame(const culpeo::message::ParsedHeadersView& f) noexcept {
    if (f.frame_type != culpeo::message::FrameType::control) {
        return std::unexpected(Error::protocol_error);
    }

    // Every control frame requires an Event header
    if (!f.event.has_value()) {
        // No Event header — protocol error. Close if established.
        std::unique_lock<std::mutex> lock(impl_->mutex);
        if (impl_->state == SessionState::established) {
            impl_->transition_to_closed_locked();
            lock.unlock();
            impl_->send_close_frame("protocol-error", "Control frame missing Event header");
            impl_->close_transport("protocol-error", "Control frame missing Event header");
        } else if (impl_->state == SessionState::uninitialized ||
                   impl_->state == SessionState::initializing) {
            impl_->state = SessionState::initializing;
            lock.unlock();
            impl_->send_init_error_and_close("protocol-error",
                "Control frame missing Event header", "{}");
            std::unique_lock<std::mutex> re(impl_->mutex);
            impl_->transition_to_closed_locked();
        }
        return std::unexpected(Error::protocol_error);
    }

    const auto event = *f.event;

    // Validate event name syntax (spec §9.5)
    if (!is_valid_event_name(event)) {
        std::unique_lock<std::mutex> lock(impl_->mutex);
        const auto st = impl_->state;
        impl_->transition_to_closed_locked();
        lock.unlock();

        if (st == SessionState::established) {
            impl_->send_close_frame("protocol-error", "Invalid event name syntax");
            impl_->close_transport("protocol-error", "Invalid event name syntax");
        } else {
            impl_->send_init_error_and_close("protocol-error",
                "Invalid event name syntax", "{}");
        }
        return std::unexpected(Error::invalid_event_name);
    }

    // Reject malformed culpeo.* variants (spec §9.5, §10.2)
    // E.g. "culpeo..init", "CULPEO.init", "culpeo.init " already caught by syntax check
    // Unknown but well-formed culpeo.* → protocol error per §10.2
    // Known culpeo.* events handled below; unknown culpeo.* should be treated as
    // unknown future protocol events and ignored per forward-compat rule (§10.2).
    // The spec says: "A syntactically invalid event name MUST be rejected as a
    // protocol error. Only after syntax validation passes SHOULD unknown, well-formed
    // event names be ignored."
    // We implement: known culpeo.* events handled; unknown culpeo.* events ignored
    // (forward compatibility for future spec versions).

    std::unique_lock<std::mutex> lock(impl_->mutex);

    // Any frame other than culpeo.init in uninitialized state → protocol error
    if (impl_->state == SessionState::uninitialized && event != "culpeo.init") {
        impl_->state = SessionState::initializing;  // Move past uninitialized
        lock.unlock();
        impl_->send_init_error_and_close("protocol-error",
            "Expected culpeo.init as first frame", "{}");
        std::unique_lock<std::mutex> re(impl_->mutex);
        impl_->transition_to_closed_locked();
        return std::unexpected(Error::protocol_error);
    }

    // Dispatch
    if (event == "culpeo.init") {
        return impl_->handle_init(f, lock);
    }
    if (event == "culpeo.ping") {
        return impl_->handle_ping(f, lock);
    }
    if (event == "culpeo.pong") {
        return impl_->handle_pong(f, lock);
    }
    if (event == "culpeo.auth-response") {
        return impl_->handle_auth_response(f, lock);
    }
    if (event == "culpeo.close") {
        return impl_->handle_close(f, lock);
    }

    // Unknown event (application or future protocol event) → ignore
    return {};
}

// ─── process_media_frame ──────────────────────────────────────────────────────

std::expected<void, Error>
Session::process_media_frame(const culpeo::message::ParsedHeadersView& f) noexcept {
    if (f.frame_type != culpeo::message::FrameType::media) {
        return std::unexpected(Error::protocol_error);
    }

    std::unique_lock<std::mutex> lock(impl_->mutex);

    if (impl_->state != SessionState::established) {
        if (impl_->state == SessionState::uninitialized) {
            // Media frame before culpeo.init → protocol-error
            impl_->state = SessionState::initializing;
            lock.unlock();
            impl_->send_init_error_and_close("protocol-error",
                "Media frame before culpeo.init", "{}");
            std::unique_lock<std::mutex> re(impl_->mutex);
            impl_->transition_to_closed_locked();
        }
        return std::unexpected(Error::wrong_state);
    }

    // Stream-Id is required on media frames
    if (!f.stream_id.has_value()) {
        impl_->transition_to_closed_locked();
        lock.unlock();
        impl_->send_close_frame("protocol-error", "Media frame missing Stream-Id");
        impl_->close_transport("protocol-error", "Media frame missing Stream-Id");
        return std::unexpected(Error::protocol_error);
    }

    const auto stream_id = *f.stream_id;

    // Validate client is allowed to send on this stream
    auto dir_check = impl_->registry.validate_client_send(stream_id);
    if (!dir_check) {
        impl_->transition_to_closed_locked();
        lock.unlock();
        impl_->send_close_frame("protocol-error", "Invalid stream direction");
        impl_->close_transport("protocol-error", "Invalid stream direction");
        return std::unexpected(dir_check.error());
    }

    auto* stream = impl_->registry.find_mutable(stream_id);
    // stream cannot be null here (validate_client_send would have returned stream_not_found)

    // Offset header is required on media frames
    if (!f.offset.has_value()) {
        impl_->transition_to_closed_locked();
        lock.unlock();
        impl_->send_close_frame("protocol-error", "Media frame missing Offset");
        impl_->close_transport("protocol-error", "Media frame missing Offset");
        return std::unexpected(Error::protocol_error);
    }

    auto offset_parsed = parse_uint64(*f.offset);
    if (!offset_parsed) {
        impl_->transition_to_closed_locked();
        lock.unlock();
        impl_->send_close_frame("protocol-error", "Invalid Offset header");
        impl_->close_transport("protocol-error", "Invalid Offset header");
        return std::unexpected(Error::protocol_error);
    }

    // Validate offset matches next expected
    auto offset_check = check_offset(*stream, *offset_parsed);
    if (!offset_check) {
        impl_->transition_to_closed_locked();
        lock.unlock();
        impl_->send_close_frame("protocol-error", "Offset mismatch");
        impl_->close_transport("protocol-error", "Offset mismatch");
        return std::unexpected(Error::offset_mismatch);
    }

    // Validate Content-Type matches stream declaration (spec §6.2):
    // - Mandatory: media frames MUST include Content-Type (SEC-014)
    // - type/subtype compared case-insensitively
    // - parameter names compared case-insensitively
    // - parameter values compared case-sensitively
    // - parameter order is insignificant
    if (!f.content_type.has_value()) {
        impl_->transition_to_closed_locked();
        lock.unlock();
        impl_->send_close_frame("protocol-error", "Media frame missing Content-Type");
        impl_->close_transport("protocol-error", "Media frame missing Content-Type");
        return std::unexpected(Error::protocol_error);
    }

    if (!content_type_matches(stream->content_type, *f.content_type)) {
        impl_->transition_to_closed_locked();
        lock.unlock();
        impl_->send_close_frame("protocol-error", "Content-Type mismatch");
        impl_->close_transport("protocol-error", "Content-Type mismatch");
        return std::unexpected(Error::content_type_mismatch);
    }

    // Parse timestamp (optional but must be valid if present)
    uint64_t timestamp_us = 0;
    if (f.timestamp.has_value()) {
        auto ts_parsed = parse_uint64(*f.timestamp);
        if (!ts_parsed) {
            impl_->transition_to_closed_locked();
            lock.unlock();
            impl_->send_close_frame("protocol-error", "Invalid Timestamp header");
            impl_->close_transport("protocol-error", "Invalid Timestamp header");
            return std::unexpected(Error::protocol_error);
        }
        timestamp_us = *ts_parsed;
    }

    // Advance offset
    const auto body_bytes = f.body_bytes();
    auto adv = advance_offset(*stream, body_bytes.size());
    if (!adv) {
        impl_->transition_to_closed_locked();
        lock.unlock();
        impl_->send_close_frame("protocol-error", "Offset overflow");
        impl_->close_transport("protocol-error", "Offset overflow");
        return std::unexpected(adv.error());
    }

    // Capture callback and stream snapshot before releasing lock
    StreamInfo stream_snapshot = *stream;  // Post-advance; caller sees updated offset
    auto cb = impl_->callbacks.on_media_received;

    lock.unlock();

    if (cb) cb(stream_snapshot, timestamp_us, body_bytes);

    return {};
}

// ─── send_media ───────────────────────────────────────────────────────────────

std::expected<void, Error>
Session::send_media(std::string_view stream_id,
                    std::span<const std::byte> payload,
                    uint64_t timestamp_us) noexcept {
    std::unique_lock<std::mutex> lock(impl_->mutex);

    if (impl_->state != SessionState::established) {
        return std::unexpected(Error::wrong_state);
    }

    auto dir_check = impl_->registry.validate_server_send(stream_id);
    if (!dir_check) return std::unexpected(dir_check.error());

    auto* stream = impl_->registry.find_mutable(stream_id);

    const std::string offset_str = std::to_string(stream->offset);
    const std::string ts_str = std::to_string(timestamp_us);
    const std::string sid_str(stream_id);
    const std::string ct_str = stream->content_type;

    // Advance offset before sending (frame carries the current offset)
    const uint64_t frame_offset = stream->offset;
    auto adv = advance_offset(*stream, payload.size());
    if (!adv) {
        return std::unexpected(adv.error());
    }

    lock.unlock();

    // Serialize frame: headers + binary payload
    const std::string off_str = std::to_string(frame_offset);
    std::vector<culpeo::message::HeaderFieldView> headers{
        {"Stream-Id", sid_str},
        {"Offset", off_str},
        {"Content-Type", ct_str},
        {"Timestamp", ts_str},
    };

    auto frame = culpeo::message::serialize_frame(
        culpeo::message::FrameType::media,
        std::span<const culpeo::message::HeaderFieldView>(headers),
        payload);
    if (!frame) {
        return std::unexpected(Error::transport_error);
    }

    try {
        impl_->transport.send_binary(*frame);
    } catch (...) {
        return std::unexpected(Error::transport_error);
    }

    return {};
}

// ─── send_auth_refresh ────────────────────────────────────────────────────────

std::expected<void, Error> Session::send_auth_refresh() noexcept {
    std::unique_lock<std::mutex> lock(impl_->mutex);

    if (impl_->state != SessionState::established) {
        return std::unexpected(Error::wrong_state);
    }

    auto nonce_result = impl_->auth_mgr.generate(impl_->config.min_auth_refresh_interval_s);
    if (!nonce_result) return std::unexpected(nonce_result.error());

    std::string body = build_auth_refresh_body(*nonce_result);

    // Zero the local hex string after building body (the nonce is now in auth_mgr)
    std::fill(nonce_result->begin(), nonce_result->end(), '\0');

    lock.unlock();

    send_text_frame_noexcept(impl_->transport,
        {{"Event", "culpeo.auth-refresh"},
         {"Content-Type", "application/json"}},
        body);

    return {};
}

// ─── send_ping ────────────────────────────────────────────────────────────────

std::expected<void, Error> Session::send_ping() noexcept {
    std::unique_lock<std::mutex> lock(impl_->mutex);

    if (impl_->state != SessionState::established) {
        return std::unexpected(Error::wrong_state);
    }

    // Generate a cryptographically random 64-bit nonce (SEC-016).
    // Store the send timestamp separately for RTT computation.
    const uint64_t nonce = generate_u64_nonce();
    const int64_t send_ts = now_us();
    impl_->pending_ping_nonce = nonce;
    impl_->pending_ping_send_ts = send_ts;

    lock.unlock();

    // Ping body: {"nonce": <uint64>, "server_ts": <microseconds_since_epoch>}
    std::string body = R"({"nonce":)" + std::to_string(nonce)
                     + R"(,"server_ts":)" + std::to_string(send_ts) + '}';
    send_text_frame_noexcept(impl_->transport,
        {{"Event", "culpeo.ping"},
         {"Content-Type", "application/json"}},
        body);

    return {};
}

// ─── close ────────────────────────────────────────────────────────────────────

void Session::close(std::string_view code, std::string_view reason) noexcept {
    std::unique_lock<std::mutex> lock(impl_->mutex);

    if (impl_->state == SessionState::closed) return;

    impl_->auth_mgr.clear();
    impl_->transition_to_closed_locked();

    auto cb = impl_->callbacks.on_close;
    std::string code_str(code);
    std::string reason_str(reason);
    lock.unlock();

    impl_->send_close_frame(code_str, reason_str);
    impl_->close_transport(code_str, reason_str);

    if (cb) cb(code_str, reason_str);
}

// ─── Inspection ──────────────────────────────────────────────────────────────

SessionState Session::state() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->state;
}

std::optional<std::string> Session::session_id() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->session_id_str.empty()) return std::nullopt;
    return impl_->session_id_str;
}

std::vector<StreamInfo> Session::streams() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->registry.snapshot();
}

std::optional<PersistedSessionState> Session::export_state() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->session_id_str.empty()) return std::nullopt;

    PersistedSessionState ps{};
    ps.session_id = impl_->session_id_str;
    ps.streams = impl_->registry.snapshot();
    ps.buffer_window_ms = impl_->negotiated_buffer_window_ms;
    ps.disconnected_at = std::chrono::steady_clock::now();
    return ps;
}

}  // namespace culpeo::session
