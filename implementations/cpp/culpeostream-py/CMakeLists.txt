cmake_minimum_required(VERSION 3.28)
project(culpeostream_py LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─── pybind11 ─────────────────────────────────────────────────────────────────
# Fetched via FetchContent (v2.13.6) for consistency with the monorepo pattern.
# See DECISIONS.md §pybind11 acquisition method.

include(FetchContent)

FetchContent_Declare(
    pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG        v2.13.6
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(pybind11)

# ─── Dependency resolution ────────────────────────────────────────────────────
# When built as a subdirectory of implementations/cpp/CMakeLists.txt, all
# culpeo:: targets are already defined.  When built standalone (e.g. via
# scikit-build-core for pip install), we include the sibling library source
# trees directly.

if(NOT TARGET culpeo::message)
    message(STATUS "culpeostream-py: standalone build — adding sibling libraries")
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../libculpeo-message
                     ${CMAKE_CURRENT_BINARY_DIR}/libculpeo-message)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../libculpeo-session
                     ${CMAKE_CURRENT_BINARY_DIR}/libculpeo-session)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../libculpeo-transport-ws
                     ${CMAKE_CURRENT_BINARY_DIR}/libculpeo-transport-ws)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../libculpeo-transport-h2
                     ${CMAKE_CURRENT_BINARY_DIR}/libculpeo-transport-h2)
endif()

# ─── Extension module ─────────────────────────────────────────────────────────

pybind11_add_module(culpeostream src/bindings.cpp)

target_link_libraries(culpeostream
    PRIVATE
        culpeo::message
        culpeo::session
        culpeo::transport_ws
        culpeo::transport_h2
)

target_compile_features(culpeostream PRIVATE cxx_std_23)

target_compile_options(culpeostream
    PRIVATE
        $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:
            -Wall -Wextra -Wpedantic -Wconversion
            -Wno-unused-parameter>
)

# GCC requires an explicit flag for C++20 coroutines (used by H2 transport).
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(culpeostream PRIVATE -fcoroutines)
endif()

# ASan / UBSan support — passed through from parent or set directly.
option(CULPEO_PY_ENABLE_ASAN  "Build culpeostream-py with AddressSanitizer"   OFF)
option(CULPEO_PY_ENABLE_UBSAN "Build culpeostream-py with UndefinedBehaviorSanitizer" OFF)

if(CULPEO_PY_ENABLE_ASAN OR CULPEO_PY_ENABLE_UBSAN)
    set(_san_flags "")
    if(CULPEO_PY_ENABLE_ASAN)
        list(APPEND _san_flags -fsanitize=address)
    endif()
    if(CULPEO_PY_ENABLE_UBSAN)
        list(APPEND _san_flags -fsanitize=undefined -fno-sanitize-recover=undefined)
    endif()
    target_compile_options(culpeostream PRIVATE ${_san_flags})
    target_link_options(culpeostream    PRIVATE ${_san_flags})
endif()

# Install the extension next to the package root (for pip install -e .).
install(TARGETS culpeostream
    LIBRARY DESTINATION .
)
