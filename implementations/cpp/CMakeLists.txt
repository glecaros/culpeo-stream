cmake_minimum_required(VERSION 3.28)
project(culpeostream_cpp LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Required for static libraries to be linkable into Python extension (.so) and
# other shared libraries without relocation errors.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(CULPEO_BUILD_TESTS "Build CulpeoStream unit tests" ON)
option(CULPEO_ENABLE_FUZZ "Build libFuzzer targets" OFF)

add_subdirectory(libculpeo-message)
add_subdirectory(libculpeo-session)
add_subdirectory(libculpeo-transport-ws)
add_subdirectory(libculpeo-transport-h2)

option(CULPEO_BUILD_PYTHON_BINDINGS "Build culpeostream Python extension (requires Python 3.10+)" ON)
if(CULPEO_BUILD_PYTHON_BINDINGS)
    add_subdirectory(culpeostream-py)
endif()
