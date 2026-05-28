include(FetchContent)

# ─── nlohmann/json ────────────────────────────────────────────────────────────
# Used for parsing/serializing JSON bodies (culpeo.init, culpeo.ping, auth flows).
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
# Header-only; no tests needed
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

# ─── libculpeo-session ────────────────────────────────────────────────────────
add_library(culpeo_session
    src/session.cpp
    src/stream_registry.cpp
    src/offset_tracker.cpp
    src/auth_refresh.cpp
)

add_library(culpeo::session ALIAS culpeo_session)

target_include_directories(culpeo_session
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

find_package(OpenSSL REQUIRED)

target_link_libraries(culpeo_session
    PUBLIC  culpeo::frame
    PRIVATE OpenSSL::Crypto nlohmann_json::nlohmann_json
)

target_compile_features(culpeo_session PUBLIC cxx_std_23)

target_compile_options(culpeo_session
    PRIVATE
        $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>
)

# ─── Tests ────────────────────────────────────────────────────────────────────
if(CULPEO_BUILD_TESTS)
    # Catch2 is already fetched by libculpeo-frame; FetchContent_MakeAvailable is idempotent.
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.8.0
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)

    # ── session_tests ─────────────────────────────────────────────────────────
    add_executable(session_tests
        tests/session_tests.cpp
    )
    target_link_libraries(session_tests
        PRIVATE culpeo::session Catch2::Catch2WithMain
    )
    target_include_directories(session_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_features(session_tests PRIVATE cxx_std_23)
    target_compile_options(session_tests
        PRIVATE
            $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wall -Wextra -Wpedantic>
    )
    catch_discover_tests(session_tests)

    # ── stream_registry_tests ─────────────────────────────────────────────────
    add_executable(stream_registry_tests
        tests/stream_registry_tests.cpp
        src/stream_registry.cpp
    )
    target_link_libraries(stream_registry_tests
        PRIVATE culpeo::frame Catch2::Catch2WithMain OpenSSL::Crypto
    )
    target_include_directories(stream_registry_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_features(stream_registry_tests PRIVATE cxx_std_23)
    target_compile_options(stream_registry_tests
        PRIVATE
            $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wall -Wextra -Wpedantic>
    )
    catch_discover_tests(stream_registry_tests)

    # ── offset_tracker_tests ──────────────────────────────────────────────────
    add_executable(offset_tracker_tests
        tests/offset_tracker_tests.cpp
        src/offset_tracker.cpp
    )
    target_link_libraries(offset_tracker_tests
        PRIVATE culpeo::frame Catch2::Catch2WithMain
    )
    target_include_directories(offset_tracker_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_features(offset_tracker_tests PRIVATE cxx_std_23)
    target_compile_options(offset_tracker_tests
        PRIVATE
            $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wall -Wextra -Wpedantic>
    )
    catch_discover_tests(offset_tracker_tests)
endif()
