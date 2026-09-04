cmake_minimum_required(VERSION 3.22.1)
project(track_n_race_android LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(TNR_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
set(TNRP_USE_QT OFF CACHE BOOL "" FORCE)
add_subdirectory(
    "${TNR_REPO_ROOT}/protocol_parser_library"
    "${CMAKE_CURRENT_BINARY_DIR}/tnrp"
)

add_library(tracknrace_android SHARED native_bridge.cpp)
target_link_libraries(tracknrace_android PRIVATE tnrp android log)
target_compile_definitions(tracknrace_android PRIVATE _FILE_OFFSET_BITS=64)

# NDK r27 still defaults ELF LOAD segments to 4 KiB. Android 15+ devices can
# use 16 KiB memory pages, so align the final JNI library for both page sizes.
target_link_options(tracknrace_android PRIVATE
    "-Wl,-z,max-page-size=16384"
    "-Wl,-z,common-page-size=16384"
)
