#include <catch2/catch_test_macros.hpp>

#include "../src/stream_registry.hpp"

#include <set>
#include <string>

using namespace culpeo::session;
using namespace culpeo::session::internal;

// ─── validate_declarations ────────────────────────────────────────────────────

TEST_CASE("validate_declarations: empty list → invalid_streams", "[stream_registry]") {
    std::vector<StreamDeclaration> decls;
    auto result = validate_declarations(decls, 16);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::invalid_streams);
}

TEST_CASE("validate_declarations: exceeds max_streams → max_streams_exceeded", "[stream_registry]") {
    std::vector<StreamDeclaration> decls;
    for (int i = 0; i < 17; ++i) {
        StreamDeclaration d{};
        d.content_type = "audio/pcm;rate=16000;channels=1;bits=16";
        d.direction = StreamDirection::input;
        d.purpose = "stream" + std::to_string(i);
        decls.push_back(d);
    }
    auto result = validate_declarations(decls, 16);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::max_streams_exceeded);
}

TEST_CASE("validate_declarations: exactly max_streams → ok", "[stream_registry]") {
    std::vector<StreamDeclaration> decls;
    for (int i = 0; i < 16; ++i) {
        StreamDeclaration d{};
        d.content_type = "audio/pcm;rate=16000;channels=1;bits=16";
        d.direction = StreamDirection::input;
        d.purpose = "stream" + std::to_string(i);
        decls.push_back(d);
    }
    auto result = validate_declarations(decls, 16);
    CHECK(result.has_value());
}

TEST_CASE("validate_declarations: single stream without purpose → ok", "[stream_registry]") {
    StreamDeclaration d{};
    d.content_type = "audio/pcm;rate=16000;channels=1;bits=16";
    d.direction = StreamDirection::input;
    auto result = validate_declarations({d}, 16);
    CHECK(result.has_value());
}

TEST_CASE("validate_declarations: two input streams without purpose → invalid", "[stream_registry]") {
    std::vector<StreamDeclaration> decls;
    StreamDeclaration d1{};
    d1.content_type = "audio/pcm;rate=16000;channels=1;bits=16";
    d1.direction = StreamDirection::input;

    StreamDeclaration d2{};
    d2.content_type = "audio/pcm;rate=44100;channels=2;bits=16";
    d2.direction = StreamDirection::input;

    auto result = validate_declarations({d1, d2}, 16);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::invalid_streams);
}

TEST_CASE("validate_declarations: two input streams with unique purposes → ok", "[stream_registry]") {
    StreamDeclaration d1{};
    d1.content_type = "audio/pcm;rate=16000;channels=1;bits=16";
    d1.direction = StreamDirection::input;
    d1.purpose = "user-voice";

    StreamDeclaration d2{};
    d2.content_type = "audio/pcm;rate=44100;channels=2;bits=16";
    d2.direction = StreamDirection::input;
    d2.purpose = "background-music";

    auto result = validate_declarations({d1, d2}, 16);
    CHECK(result.has_value());
}

TEST_CASE("validate_declarations: duplicate purpose within type → invalid", "[stream_registry]") {
    StreamDeclaration d1{};
    d1.content_type = "audio/pcm;rate=16000;channels=1;bits=16";
    d1.direction = StreamDirection::input;
    d1.purpose = "user-voice";

    StreamDeclaration d2{};
    d2.content_type = "audio/pcm;rate=44100;channels=2;bits=16";
    d2.direction = StreamDirection::input;
    d2.purpose = "user-voice";  // Duplicate!

    auto result = validate_declarations({d1, d2}, 16);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::invalid_streams);
}

TEST_CASE("validate_declarations: duplicate purpose different types → ok", "[stream_registry]") {
    // Same purpose string but different directions is allowed
    StreamDeclaration d1{};
    d1.content_type = "audio/pcm;rate=16000;channels=1;bits=16";
    d1.direction = StreamDirection::input;
    d1.purpose = "audio";

    StreamDeclaration d2{};
    d2.content_type = "audio/opus";
    d2.direction = StreamDirection::output;
    d2.purpose = "audio";

    auto result = validate_declarations({d1, d2}, 16);
    CHECK(result.has_value());
}

TEST_CASE("validate_declarations: empty content_type → invalid", "[stream_registry]") {
    StreamDeclaration d{};
    d.content_type = "";
    d.direction = StreamDirection::input;

    auto result = validate_declarations({d}, 16);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::invalid_streams);
}

// ─── StreamRegistry ───────────────────────────────────────────────────────────

TEST_CASE("StreamRegistry: register and find streams", "[stream_registry]") {
    StreamRegistry reg(16);

    StreamDeclaration d{};
    d.content_type = "audio/pcm;rate=16000;channels=1;bits=16";
    d.direction = StreamDirection::input;
    d.purpose = "user-voice";

    auto result = reg.register_from_declarations({d});
    REQUIRE(result.has_value());

    auto snapshot = reg.snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].content_type == "audio/pcm;rate=16000;channels=1;bits=16");
    CHECK(snapshot[0].direction == StreamDirection::input);
    CHECK(snapshot[0].purpose == "user-voice");
    CHECK(snapshot[0].offset == 0);
    CHECK(!snapshot[0].id.empty());
    CHECK(snapshot[0].id.size() == 16);  // 8 bytes hex = 16 chars
}

TEST_CASE("StreamRegistry: find by ID", "[stream_registry]") {
    StreamRegistry reg(16);

    StreamDeclaration d{};
    d.content_type = "audio/opus";
    d.direction = StreamDirection::output;

    auto result = reg.register_from_declarations({d});
    REQUIRE(result.has_value());

    auto snapshot = reg.snapshot();
    const std::string id = snapshot[0].id;

    const auto* found = reg.find(id);
    REQUIRE(found != nullptr);
    CHECK(found->id == id);

    const auto* not_found = reg.find("nonexistent-id");
    CHECK(not_found == nullptr);
}

TEST_CASE("StreamRegistry: multiple streams get unique IDs", "[stream_registry]") {
    StreamRegistry reg(16);

    std::vector<StreamDeclaration> decls;
    for (int i = 0; i < 4; ++i) {
        StreamDeclaration d{};
        d.content_type = "audio/opus";
        d.direction = StreamDirection::input;
        d.purpose = "stream" + std::to_string(i);
        decls.push_back(d);
    }

    auto result = reg.register_from_declarations(decls);
    REQUIRE(result.has_value());

    auto snapshot = reg.snapshot();
    REQUIRE(snapshot.size() == 4);

    // All IDs should be unique
    std::set<std::string> ids;
    for (const auto& s : snapshot) {
        ids.insert(s.id);
    }
    CHECK(ids.size() == 4);
}

TEST_CASE("StreamRegistry: validate_server_send allows output stream", "[stream_registry]") {
    StreamRegistry reg(16);
    StreamDeclaration d{};
    d.content_type = "audio/opus";
    d.direction = StreamDirection::output;
    reg.register_from_declarations({d});

    auto snapshot = reg.snapshot();
    auto result = reg.validate_server_send(snapshot[0].id);
    CHECK(result.has_value());
}

TEST_CASE("StreamRegistry: validate_server_send rejects input stream", "[stream_registry]") {
    StreamRegistry reg(16);
    StreamDeclaration d{};
    d.content_type = "audio/opus";
    d.direction = StreamDirection::input;
    reg.register_from_declarations({d});

    auto snapshot = reg.snapshot();
    auto result = reg.validate_server_send(snapshot[0].id);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::invalid_direction);
}

TEST_CASE("StreamRegistry: validate_server_send allows duplex", "[stream_registry]") {
    StreamRegistry reg(16);
    StreamDeclaration d{};
    d.content_type = "application/json";
    d.direction = StreamDirection::duplex;
    reg.register_from_declarations({d});

    auto snapshot = reg.snapshot();
    auto result = reg.validate_server_send(snapshot[0].id);
    CHECK(result.has_value());
}

TEST_CASE("StreamRegistry: validate_client_send allows input stream", "[stream_registry]") {
    StreamRegistry reg(16);
    StreamDeclaration d{};
    d.content_type = "audio/pcm;rate=16000;channels=1;bits=16";
    d.direction = StreamDirection::input;
    reg.register_from_declarations({d});

    auto snapshot = reg.snapshot();
    auto result = reg.validate_client_send(snapshot[0].id);
    CHECK(result.has_value());
}

TEST_CASE("StreamRegistry: validate_client_send rejects output stream", "[stream_registry]") {
    StreamRegistry reg(16);
    StreamDeclaration d{};
    d.content_type = "audio/opus";
    d.direction = StreamDirection::output;
    reg.register_from_declarations({d});

    auto snapshot = reg.snapshot();
    auto result = reg.validate_client_send(snapshot[0].id);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == Error::invalid_direction);
}

TEST_CASE("StreamRegistry: validate_* unknown ID → stream_not_found", "[stream_registry]") {
    StreamRegistry reg(16);
    StreamDeclaration d{};
    d.content_type = "audio/opus";
    d.direction = StreamDirection::input;
    reg.register_from_declarations({d});

    auto r1 = reg.validate_server_send("unknown");
    REQUIRE_FALSE(r1.has_value());
    CHECK(r1.error() == Error::stream_not_found);

    auto r2 = reg.validate_client_send("unknown");
    REQUIRE_FALSE(r2.has_value());
    CHECK(r2.error() == Error::stream_not_found);
}

TEST_CASE("StreamRegistry: codec detection for PCM", "[stream_registry]") {
    StreamRegistry reg(16);
    StreamDeclaration d{};
    d.content_type = "audio/pcm;rate=16000;channels=1;bits=16";
    d.direction = StreamDirection::input;
    reg.register_from_declarations({d});

    auto snapshot = reg.snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].codec == StreamCodec::pcm);
    REQUIRE(snapshot[0].pcm_params.has_value());
    CHECK(snapshot[0].pcm_params->rate == 16000);
    CHECK(snapshot[0].pcm_params->channels == 1);
    CHECK(snapshot[0].pcm_params->bits == 16);
}

TEST_CASE("StreamRegistry: codec detection for Opus", "[stream_registry]") {
    StreamRegistry reg(16);
    StreamDeclaration d{};
    d.content_type = "audio/opus";
    d.direction = StreamDirection::output;
    reg.register_from_declarations({d});

    auto snapshot = reg.snapshot();
    CHECK(snapshot[0].codec == StreamCodec::opus);
    CHECK_FALSE(snapshot[0].pcm_params.has_value());
}

TEST_CASE("StreamRegistry: register_from_persisted restores state", "[stream_registry]") {
    StreamRegistry reg(16);

    StreamInfo si{};
    si.id = "test-stream-id";
    si.content_type = "audio/opus";
    si.direction = StreamDirection::input;
    si.purpose = "audio";
    si.offset = 42;
    si.codec = StreamCodec::opus;

    reg.register_from_persisted({si});

    const auto* found = reg.find("test-stream-id");
    REQUIRE(found != nullptr);
    CHECK(found->offset == 42);
    CHECK(found->codec == StreamCodec::opus);
}

TEST_CASE("StreamRegistry: clear removes all streams", "[stream_registry]") {
    StreamRegistry reg(16);
    StreamDeclaration d{};
    d.content_type = "audio/opus";
    d.direction = StreamDirection::input;
    reg.register_from_declarations({d});
    CHECK_FALSE(reg.empty());

    reg.clear();
    CHECK(reg.empty());
    CHECK(reg.snapshot().empty());
}

// ─── parse_stream_direction ───────────────────────────────────────────────────

TEST_CASE("parse_stream_direction: valid strings", "[stream_registry]") {
    CHECK(parse_stream_direction("input")  == StreamDirection::input);
    CHECK(parse_stream_direction("output") == StreamDirection::output);
    CHECK(parse_stream_direction("duplex") == StreamDirection::duplex);
}

TEST_CASE("parse_stream_direction: invalid string → nullopt", "[stream_registry]") {
    CHECK_FALSE(parse_stream_direction("Input").has_value());
    CHECK_FALSE(parse_stream_direction("INPUT").has_value());
    CHECK_FALSE(parse_stream_direction("").has_value());
    CHECK_FALSE(parse_stream_direction("bidirectional").has_value());
}
