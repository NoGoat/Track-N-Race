cmake_minimum_required(VERSION 3.16)
project(tnrp VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Dual-mode UDP backend. OFF (default) => raw winsock2/POSIX sockets, for the
# headless bridge. ON => QUdpSocket, for linking into the native Qt recorder.
option(TNRP_USE_QT "Use QUdpSocket instead of raw sockets for UDP" OFF)
option(TNRP_BUILD_TESTS "Build libtnrp native regression tests" OFF)

include(FetchContent)

# All JSON (serialise + parse) goes through glaze. zlib remains for TNRD V1
# gzip compatibility; chunked Zstandard is the default TNRD V4 codec.
if(NOT TARGET glaze::glaze)
    FetchContent_Declare(glaze
        GIT_REPOSITORY https://github.com/stephenberry/glaze.git
        GIT_TAG v7.8.3
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(glaze)
endif()

if(NOT TARGET zlibstatic)
    # libxlsxwriter links the canonical ZLIB::ZLIB target. zlib 1.3.2 maps
    # that name to its shared target when shared builds are enabled, which
    # makes Windows consumers import z.dll even though tnrp itself requests
    # zlibstatic. Build only the static variant so our FindZLIB shim below can
    # consistently expose ZLIB::ZLIB as zlibstatic.
    set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
    FetchContent_Declare(zlib
        GIT_REPOSITORY https://github.com/madler/zlib.git
        GIT_TAG v1.3.2
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(zlib)
endif()

if(NOT TARGET libzstd_static)
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
    set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
    set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)

    if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.18")
        FetchContent_Declare(zstd
            GIT_REPOSITORY https://github.com/facebook/zstd.git
            GIT_TAG v1.5.7
            GIT_SHALLOW TRUE
            SOURCE_SUBDIR build/cmake)
        FetchContent_MakeAvailable(zstd)
    else()
        FetchContent_Declare(zstd
            GIT_REPOSITORY https://github.com/facebook/zstd.git
            GIT_TAG v1.5.7
            GIT_SHALLOW TRUE)
        FetchContent_GetProperties(zstd)
        if(NOT zstd_POPULATED)
            FetchContent_Populate(zstd)
        endif()
        add_subdirectory(${zstd_SOURCE_DIR}/build/cmake ${zstd_BINARY_DIR})
    endif()
endif()

# XLSX export (raw-data dump of a recorded session). libxlsxwriter's own
# CMakeLists.txt calls find_package(ZLIB REQUIRED); shadow CMake's bundled
# FindZLIB.cmake with cmake/FindZLIB.cmake so that resolves to the zlib we
# already fetched above (target `zlibstatic`) instead of searching for a
# system zlib. CMAKE_MODULE_PATH must be updated before FetchContent's
# add_subdirectory() runs below.
if(NOT TARGET xlsxwriter)
    list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(USE_SYSTEM_MINIZIP OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(libxlsxwriter
        GIT_REPOSITORY https://github.com/jmcnamara/libxlsxwriter.git
        GIT_TAG v1.2.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(libxlsxwriter)
endif()

add_library(tnrp STATIC
    src/protocols/protocol.cpp
    src/protocols/f1_24.cpp
    src/protocols/f1_25.cpp
    src/protocols/f1_26.cpp
    src/TimeUtils.cpp
    src/DisplayNames.cpp
    src/Labels.cpp
    src/CardColors.cpp
    src/Parser.cpp
    src/AnyRow.cpp
    src/TnrdCodec.cpp
    src/TnrdV4.cpp
    src/TnrdWriter.cpp
    src/TnrdReader.cpp
    src/UdpListener.cpp
    src/Engine.cpp
    src/XlsxExport.cpp
)

# Public headers (tnrp/*.h) under include/; internal headers (protocols/*) under src/.
target_include_directories(tnrp
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# glaze is in public headers. Both compression libraries are implementation
# details, but static-library consumers still receive their link dependencies.
target_link_libraries(tnrp PUBLIC glaze::glaze xlsxwriter PRIVATE zlibstatic libzstd_static)

# zlib's generated zconf.h lives in its build dir; expose it to consumers too.
target_include_directories(tnrp PUBLIC
    $<BUILD_INTERFACE:${zlib_SOURCE_DIR}>
    $<BUILD_INTERFACE:${zlib_BINARY_DIR}>)

if(TNRP_USE_QT)
    find_package(Qt6 QUIET COMPONENTS Core Network)
    if(NOT Qt6_FOUND)
        find_package(Qt5 REQUIRED COMPONENTS Core Network)
        target_link_libraries(tnrp PUBLIC Qt5::Core Qt5::Network)
    else()
        target_link_libraries(tnrp PUBLIC Qt6::Core Qt6::Network)
    endif()
    target_compile_definitions(tnrp PUBLIC TNRP_USE_QT)
    message(STATUS "tnrp: UDP backend = QUdpSocket (Qt)")
else()
    if(WIN32)
        target_link_libraries(tnrp PUBLIC ws2_32)
    endif()
    message(STATUS "tnrp: UDP backend = raw sockets")
endif()

# Match the recorder's optimization posture for the hot parse/record path.
if(NOT MSVC)
    target_compile_options(tnrp PRIVATE $<$<NOT:$<CONFIG:Debug>>:-O3 -fno-math-errno>)
else()
    target_compile_options(tnrp PRIVATE $<$<NOT:$<CONFIG:Debug>>:/O2>)
    target_compile_definitions(tnrp PRIVATE NOMINMAX)
endif()

if(TNRP_BUILD_TESTS)
    enable_testing()
    add_executable(tnrd_v4_roundtrip tests/TnrdV4RoundTrip.cpp)
    target_include_directories(tnrd_v4_roundtrip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(tnrd_v4_roundtrip PRIVATE tnrp)
    add_test(NAME tnrd_v4_roundtrip COMMAND tnrd_v4_roundtrip)
    add_executable(parser_rewind tests/ParserRewind.cpp)
    target_link_libraries(parser_rewind PRIVATE tnrp)
    add_test(NAME parser_rewind COMMAND parser_rewind)
    add_executable(v4_lap_boundary tests/V4LapBoundary.cpp)
    target_include_directories(v4_lap_boundary PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(v4_lap_boundary PRIVATE tnrp)
    add_test(NAME v4_lap_boundary COMMAND v4_lap_boundary)
endif()
