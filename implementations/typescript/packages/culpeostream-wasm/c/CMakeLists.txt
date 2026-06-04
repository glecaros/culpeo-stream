cmake_minimum_required(VERSION 3.16)
project(culpeo_parser C)

set(CMAKE_C_STANDARD 99)

# When invoked via emcmake, CMAKE_C_COMPILER is set to emcc automatically.
add_executable(culpeo_parser c/culpeo_parser.c)

set_target_properties(culpeo_parser PROPERTIES
    SUFFIX ".js"
    OUTPUT_NAME "culpeo_parser"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/dist"
)

target_compile_options(culpeo_parser PRIVATE -O2)

target_link_options(culpeo_parser PRIVATE
    "-sWASM=1"
    "-sEXPORTED_FUNCTIONS=[\"_culpeo_parse_headers\",\"_culpeo_serialize_frame\",\"_malloc\",\"_free\"]"
    "-sEXPORTED_RUNTIME_METHODS=[\"ccall\",\"cwrap\",\"HEAPU8\",\"getValue\",\"setValue\"]"
    "-sALLOW_MEMORY_GROWTH=1"
    "--no-entry"
    "-sMODULARIZE=1"
    "-sEXPORT_NAME=createCulpeoParserModule"
)
