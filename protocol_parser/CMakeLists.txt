cmake_minimum_required(VERSION 3.16)
project(protocol_parser VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# The headless bridge always uses the raw-socket UDP backend (no Qt).
set(TNRP_USE_QT OFF CACHE BOOL "" FORCE)

# Build libtnrp from the sibling library directory (supports standalone builds;
# the top-level CMakeLists also adds it).
if(NOT TARGET tnrp)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../protocol_parser_library
                     ${CMAKE_BINARY_DIR}/tnrp_build)
endif()

add_executable(protocol_parser
    src/main.cpp
    src/PipeSink.cpp
    src/StdinControl.cpp
)
target_link_libraries(protocol_parser PRIVATE tnrp)
set_target_properties(protocol_parser PROPERTIES OUTPUT_NAME "protocol_parser")

if(NOT MSVC)
    target_compile_options(protocol_parser PRIVATE $<$<NOT:$<CONFIG:Debug>>:-O3>)
else()
    target_compile_options(protocol_parser PRIVATE $<$<NOT:$<CONFIG:Debug>>:/O2>)
    target_compile_definitions(protocol_parser PRIVATE NOMINMAX)
endif()
