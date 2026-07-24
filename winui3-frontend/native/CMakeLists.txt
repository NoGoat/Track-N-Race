cmake_minimum_required(VERSION 3.20)
project(TrackNRaceWinUI3Bridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# WinUI is a non-Qt host, so libtnrp owns a raw Winsock UDP listener.
set(TNRP_USE_QT OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../protocol_parser_library
                 ${CMAKE_BINARY_DIR}/tnrp_build)

add_library(track_n_race_engine_bridge SHARED
    src/EngineBridge.cpp
    src/EngineBridge.h)

target_compile_definitions(track_n_race_engine_bridge PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(track_n_race_engine_bridge PRIVATE tnrp)

if(MSVC)
    # MSBuild compiles target sources in parallel. Serialize writes to each
    # target's Debug PDB so libtnrp's translation units do not race on C1041.
    target_compile_options(tnrp PRIVATE /FS)
    target_compile_options(track_n_race_engine_bridge PRIVATE /FS)
endif()

set_target_properties(track_n_race_engine_bridge PROPERTIES
    OUTPUT_NAME "TrackNRace.EngineBridge")
