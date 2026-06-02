cmake_minimum_required(VERSION 3.28)

# ── Dependencies ──────────────────────────────────────────────────────────────

find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)

# ── nghttp2: prefer system package, fall back to FetchContent ─────────────────
pkg_check_modules(NGHTTP2 libnghttp2)

if(NOT NGHTTP2_FOUND)
    message(STATUS "nghttp2 system package not found; fetching v1.62.1 via FetchContent")
    include(FetchContent)
    FetchContent_Declare(nghttp2
        GIT_REPOSITORY https://github.com/nghttp2/nghttp2.git
        GIT_TAG        v1.62.1
        GIT_SHALLOW    TRUE
    )
    set(ENABLE_LIB_ONLY        ON  CACHE BOOL "" FORCE)
    set(ENABLE_STATIC_LIB      ON  CACHE BOOL "" FORCE)
    set(ENABLE_SHARED_LIB      OFF CACHE BOOL "" FORCE)
    set(ENABLE_ASIO_LIB        OFF CACHE BOOL "" FORCE)
    set(ENABLE_EXAMPLES        OFF CACHE BOOL "" FORCE)
    set(ENABLE_APP             OFF CACHE BOOL "" FORCE)
    set(ENABLE_HPACK_TOOLS     OFF CACHE BOOL "" FORCE)
    set(ENABLE_PYTHON_BINDINGS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(nghttp2)
    set(NGHTTP2_LIBRARIES    nghttp2_static)
    set(NGHTTP2_INCLUDE_DIRS ${nghttp2_SOURCE_DIR}/lib/includes
                             ${nghttp2_BINARY_DIR}/lib/includes)
endif()

# ── Asio: prefer system package, fall back to FetchContent ───────────────────
find_path(ASIO_INCLUDE_DIR asio.hpp
    PATHS /usr/include /usr/local/include
    NO_DEFAULT_PATH)

if(NOT ASIO_INCLUDE_DIR)
    message(STATUS "Asio system headers not found; fetching via FetchContent")
    include(FetchContent)
    FetchContent_Declare(asio
        GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
        GIT_TAG        asio-1-30-2
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(asio)
    set(ASIO_INCLUDE_DIR ${asio_SOURCE_DIR}/asio/include)
endif()

# Asio standalone interface target (reuse if already defined by another subdir)
if(NOT TARGET asio_standalone)
    add_library(asio_standalone INTERFACE)
    target_include_directories(asio_standalone INTERFACE ${ASIO_INCLUDE_DIR})
    target_compile_definitions(asio_standalone INTERFACE
        ASIO_STANDALONE
        ASIO_NO_DEPRECATED)
endif()

# ── libculpeo-transport-h2 ────────────────────────────────────────────────────

add_library(culpeo_transport_h2
    src/h2_transport.cpp
    src/h2_client.cpp
    src/h2_server.cpp
)

add_library(culpeo::transport_h2 ALIAS culpeo_transport_h2)

target_include_directories(culpeo_transport_h2
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${NGHTTP2_INCLUDE_DIRS}
        src  # internal headers in src/
)

target_link_libraries(culpeo_transport_h2
    PUBLIC
        culpeo::session
        asio_standalone
        OpenSSL::SSL
        OpenSSL::Crypto
    PRIVATE
        ${NGHTTP2_LIBRARIES}
)

target_compile_features(culpeo_transport_h2 PUBLIC cxx_std_23)

target_compile_options(culpeo_transport_h2
    PRIVATE
        $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:
            -Wall -Wextra -Wpedantic -Wconversion
            -Wno-unused-parameter>
)

# Coroutine support
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(culpeo_transport_h2 PRIVATE -fcoroutines)
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────

if(CULPEO_BUILD_TESTS)
    # Catch2 is already fetched by libculpeo-message; idempotent
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.8.0
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)

    add_executable(h2_transport_tests
        tests/h2_transport_tests.cpp
    )

    target_link_libraries(h2_transport_tests
        PRIVATE
            culpeo::transport_h2
            Catch2::Catch2WithMain
    )

    target_compile_features(h2_transport_tests PRIVATE cxx_std_23)

    target_compile_options(h2_transport_tests
        PRIVATE
            $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:
                -Wall -Wextra -Wpedantic
                -Wno-unused-parameter>
    )

    # SEC-023 / SEC-029: point the test binary at the directory containing
    # test_cert.pem and test_key.pem (generated once by openssl in tests/).
    target_compile_definitions(h2_transport_tests
        PRIVATE
            "TEST_CERT_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/\""
    )

    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(h2_transport_tests PRIVATE -fcoroutines)
    endif()

    catch_discover_tests(h2_transport_tests
        PROPERTIES TIMEOUT 30)
endif()
