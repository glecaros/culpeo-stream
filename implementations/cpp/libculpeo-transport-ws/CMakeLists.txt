include(FetchContent)

# ─── uWebSockets headers (optional, for uws_adapter.hpp) ─────────────────────
#
# uWebSockets is a header-only C++ WebSocket library (Apache 2.0).
# Only the C++ headers are fetched here; uSockets (the underlying C runtime)
# must be compiled and linked separately by the consuming project.
#
# The core culpeo_transport_ws library does NOT link against uWebSockets or
# uSockets — WsTransport uses injected std::function callbacks and is
# entirely library-agnostic.  The uws_adapter.hpp header is provided for
# projects that choose the uWebSockets backend.
#
# To use the uWS adapter in your project:
#   1. Build and link uSockets (https://github.com/uNetworking/uSockets)
#   2. Include the uWebSockets header directory
#   3. Include <culpeo/uws_adapter.hpp> in your WebSocket handler
#
option(CULPEO_FETCH_UWS_HEADERS
    "Fetch uWebSockets headers for uws_adapter.hpp (no compilation of uSockets)" ON)

if(CULPEO_FETCH_UWS_HEADERS)
    FetchContent_Declare(
        uWebSockets
        GIT_REPOSITORY https://github.com/uNetworking/uWebSockets.git
        GIT_TAG        v20.62.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(uWebSockets)

    # uWebSockets is header-only C++ — expose only as an INTERFACE target.
    # Consumers of uws_adapter.hpp must also link against uSockets themselves.
    add_library(uwebsockets_headers INTERFACE)
    target_include_directories(uwebsockets_headers
        INTERFACE ${uwebsockets_SOURCE_DIR}/src)
endif()

# ─── libculpeo-transport-ws ───────────────────────────────────────────────────

add_library(culpeo_transport_ws
    src/transport_ws.cpp
)

add_library(culpeo::transport_ws ALIAS culpeo_transport_ws)

target_include_directories(culpeo_transport_ws
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(culpeo_transport_ws
    PUBLIC culpeo::session
)

target_compile_features(culpeo_transport_ws PUBLIC cxx_std_23)

target_compile_options(culpeo_transport_ws
    PRIVATE
        $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>
)

# ─── Tests ────────────────────────────────────────────────────────────────────

if(CULPEO_BUILD_TESTS)
    # Catch2 is already fetched by libculpeo-message; FetchContent_MakeAvailable is idempotent.
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.8.0
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)

    add_executable(transport_ws_tests
        tests/transport_ws_tests.cpp
    )

    target_link_libraries(transport_ws_tests
        PRIVATE culpeo::transport_ws Catch2::Catch2WithMain
    )

    target_compile_features(transport_ws_tests PRIVATE cxx_std_23)

    target_compile_options(transport_ws_tests
        PRIVATE
            $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wall -Wextra -Wpedantic>
    )

    catch_discover_tests(transport_ws_tests)
endif()
