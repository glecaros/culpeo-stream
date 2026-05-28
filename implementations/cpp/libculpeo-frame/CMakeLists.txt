add_library(culpeo_frame
    src/frame.cpp
)

add_library(culpeo::frame ALIAS culpeo_frame)

target_include_directories(culpeo_frame
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(culpeo_frame PUBLIC cxx_std_23)

target_compile_options(culpeo_frame
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

    add_executable(frame_tests
        tests/frame_tests.cpp
    )

    target_link_libraries(frame_tests PRIVATE culpeo::frame Catch2::Catch2WithMain)
    target_compile_features(frame_tests PRIVATE cxx_std_23)
    target_compile_options(frame_tests
        PRIVATE
            $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>
    )

    catch_discover_tests(frame_tests)
endif()

if(CULPEO_ENABLE_FUZZ)
    add_executable(frame_parser_fuzz
        fuzz/frame_parser_fuzz.cpp
    )

    target_link_libraries(frame_parser_fuzz PRIVATE culpeo::frame)
    target_compile_features(frame_parser_fuzz PRIVATE cxx_std_23)
    target_compile_options(frame_parser_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(frame_parser_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
