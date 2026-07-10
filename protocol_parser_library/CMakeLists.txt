cmake_minimum_required(VERSION 3.16)
project(tnrp VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Dual-mode UDP backend. OFF (default) => raw winsock2/POSIX sockets, for the
# headless bridge. ON => QUdpSocket, for linking into the native Qt recorder.
option(TNRP_USE_QT "Use QUdpSocket instead of raw sockets for UDP" OFF)

include(FetchContent)

# All JSON (serialise + parse) goes through glaze; zlib is reused from a parent
# project if present, otherwise fetched at the version the native recorder pins.
if(NOT TARGET glaze::glaze)
    FetchContent_Declare(glaze
        GIT_REPOSITORY https://github.com/stephenberry/glaze.git
        GIT_TAG v7.8.3
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(glaze)
endif()

if(NOT TARGET zlibstatic)
    FetchContent_Declare(zlib
        GIT_REPOSITORY https://github.com/madler/zlib.git
        GIT_TAG v1.3.2
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(zlib)
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
    src/Labels.cpp
    src/CardColors.cpp
    src/Parser.cpp
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

# glaze is in public headers (rows/control_rows/TnrdReader); zlib is in TnrdWriter.h.
target_link_libraries(tnrp PUBLIC zlibstatic glaze::glaze xlsxwriter)

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
