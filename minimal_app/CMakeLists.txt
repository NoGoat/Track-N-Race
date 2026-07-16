cmake_minimum_required(VERSION 3.20)
project(TrackNRaceMinimalRecorder VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
    # Keep the executable and every statically linked libtnrp dependency on the
    # same CRT, producing a Windows folder with no VC runtime prerequisite.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# The minimal recorder deliberately uses libtnrp's raw Winsock/POSIX backend.
set(TNRP_USE_QT OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../protocol_parser_library
                 ${CMAKE_BINARY_DIR}/tnrp_build)

# Generate a C++ translation unit containing the canonical license texts. The
# attribution viewers read these string literals from the executable; no loose
# license files are required beside a distributed binary.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE GPL3_TEXT)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/../native_recorder/licenses/MIT-glaze.txt GLAZE_MIT_TEXT)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/../native_recorder/licenses/Zlib.txt ZLIB_LICENSE_TEXT)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/../native_recorder/licenses/BSD-3-Clause-Zstandard.txt ZSTD_BSD3_TEXT)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/../native_recorder/licenses/BSD-2-Clause-libxlsxwriter.txt LIBXLSXWRITER_BSD2_TEXT)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/../native_recorder/licenses/LGPL-3.0.txt LGPL3_TEXT)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated)
configure_file(src/Attributions.cpp.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/Attributions.cpp @ONLY)

set(COMMON_SOURCES
    src/Attributions.h
    ${CMAKE_CURRENT_BINARY_DIR}/generated/Attributions.cpp
    src/AppSettings.h
    src/MinimalController.cpp
    src/MinimalController.h)

if(WIN32)
    add_executable(minimal_app WIN32
        ${COMMON_SOURCES}
        src/windows/app.rc
        src/windows/win32_main.cpp)
    target_include_directories(minimal_app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/windows)
    target_compile_definitions(minimal_app PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(minimal_app PRIVATE
        tnrp user32 comctl32 dwmapi ole32 shell32 uxtheme windowsapp advapi32 ws2_32)
    # app.rc embeds the one manifest resource so the linker must not synthesize
    # a second one with the same resource id.
    target_link_options(minimal_app PRIVATE /MANIFEST:NO)
    set_target_properties(minimal_app PROPERTIES
        OUTPUT_NAME "Track N Race Minimal Recorder")

    install(TARGETS minimal_app RUNTIME DESTINATION .)
    return()
endif()

if(NOT UNIX OR APPLE)
    message(FATAL_ERROR "minimal_app supports Windows and desktop Linux only")
endif()

find_package(Qt6 QUIET COMPONENTS Widgets)
if(Qt6_FOUND)
    set(MINIMAL_QT_TARGET Qt6::Widgets)
    message(STATUS "Using Qt6 for the minimal recorder")
else()
    find_package(Qt5 REQUIRED COMPONENTS Widgets)
    set(MINIMAL_QT_TARGET Qt5::Widgets)
    message(STATUS "Using Qt5 for the minimal recorder (Qt6 not found)")
endif()

add_executable(minimal_app ${COMMON_SOURCES} src/linux/qt_main.cpp)
target_link_libraries(minimal_app PRIVATE tnrp ${MINIMAL_QT_TARGET})
target_compile_definitions(minimal_app PRIVATE TNR_MINIMAL_QT)
set_target_properties(minimal_app PROPERTIES OUTPUT_NAME track-n-race-minimal)

target_include_directories(minimal_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

include(GNUInstallDirs)
install(TARGETS minimal_app RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/track-n-race-minimal.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/assets/linux/track-n-race-minimal.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps
        RENAME track-n-race-minimal.png)
