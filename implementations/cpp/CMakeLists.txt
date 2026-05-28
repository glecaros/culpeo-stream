cmake_minimum_required(VERSION 3.28)
project(culpeostream_cpp LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CULPEO_BUILD_TESTS "Build CulpeoStream unit tests" ON)
option(CULPEO_ENABLE_FUZZ "Build libFuzzer targets" OFF)

add_subdirectory(libculpeo-frame)
# add_subdirectory(libculpeo-session)
