option(CULPEO_BUILD_C_API "Build the extern \"C\" API shim (for WASM/FFI use)" OFF)

set(CULPEO_MESSAGE_SOURCES src/message.cpp)
if(CULPEO_BUILD_C_API)
    list(APPEND CULPEO_MESSAGE_SOURCES src/c_api.cpp)
endif()

add_library(culpeo_message ${CULPEO_MESSAGE_SOURCES})

add_library(culpeo::message ALIAS culpeo_message)

target_include_directories(culpeo_message
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(culpeo_message PUBLIC cxx_std_23)

target_compile_options(culpeo_message
    PRIVATE
        $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>
)

if(CULPEO_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.8.0
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)

    add_executable(message_tests
        tests/message_tests.cpp
    )

    target_link_libraries(message_tests PRIVATE culpeo::message Catch2::Catch2WithMain)
    target_compile_features(message_tests PRIVATE cxx_std_23)
    target_compile_options(message_tests
        PRIVATE
            $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>
    )

    catch_discover_tests(message_tests)
endif()

if(CULPEO_ENABLE_FUZZ)
    add_executable(message_parser_fuzz
        fuzz/message_parser_fuzz.cpp
    )

    target_link_libraries(message_parser_fuzz PRIVATE culpeo::message)
    target_compile_features(message_parser_fuzz PRIVATE cxx_std_23)
    target_compile_options(message_parser_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(message_parser_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
