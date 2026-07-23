cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED TNR_APP_VERSION)
    if(DEFINED ENV{TNR_APP_VERSION} AND NOT "$ENV{TNR_APP_VERSION}" STREQUAL "")
        set(TNR_APP_VERSION "$ENV{TNR_APP_VERSION}")
    else()
        set(TNR_APP_VERSION "1.0.0")
    endif()
endif()
project(TrackNRaceMinimalRecorder VERSION "${TNR_APP_VERSION}" LANGUAGES CXX)

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
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/../qt_frontend/licenses/MIT-glaze.txt GLAZE_MIT_TEXT)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/../qt_frontend/licenses/Zlib.txt ZLIB_LICENSE_TEXT)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/../qt_frontend/licenses/BSD-3-Clause-Zstandard.txt ZSTD_BSD3_TEXT)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/../qt_frontend/licenses/BSD-2-Clause-libxlsxwriter.txt LIBXLSXWRITER_BSD2_TEXT)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/../qt_frontend/licenses/LGPL-3.0.txt LGPL3_TEXT)
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
        OUTPUT_NAME "Track-N-Race - Minimal")

    install(TARGETS minimal_app RUNTIME DESTINATION .)
    return()
endif()

if(APPLE)
    enable_language(OBJCXX)

    set(MINIMAL_MACOS_DEPLOYMENT_TARGET "13.0" CACHE STRING
        "Oldest macOS version supported by the SwiftUI frontend")
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "${MINIMAL_MACOS_DEPLOYMENT_TARGET}" CACHE STRING "" FORCE)
    endif()
    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}" CACHE STRING "" FORCE)
    endif()
    list(LENGTH CMAKE_OSX_ARCHITECTURES MINIMAL_MACOS_ARCH_COUNT)
    if(NOT MINIMAL_MACOS_ARCH_COUNT EQUAL 1)
        message(FATAL_ERROR
            "The SwiftUI build accepts one CMAKE_OSX_ARCHITECTURES value at a time. "
            "Build arm64 and x86_64 separately before combining distribution artifacts.")
    endif()
    list(GET CMAKE_OSX_ARCHITECTURES 0 MINIMAL_MACOS_ARCH)

    find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
    find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_program(XCRUN_EXECUTABLE xcrun REQUIRED)
    execute_process(
        COMMAND "${XCRUN_EXECUTABLE}" --sdk macosx --show-sdk-path
        OUTPUT_VARIABLE MINIMAL_MACOS_SDK
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    execute_process(
        COMMAND "${XCRUN_EXECUTABLE}" --find swiftc
        OUTPUT_VARIABLE MINIMAL_SWIFTC
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)

    # CMake's Swift language backend requires Ninja or full Xcode. Build the
    # small SwiftUI module with the Swift driver directly so Apple's standalone
    # Command Line Tools are sufficient. The Objective-C++ bridge is a private
    # dylib containing the recorder and all of its static native dependencies.
    add_library(minimal_macos_bridge SHARED
        ${COMMON_SOURCES}
        src/macos/TNRRecorderBridge.h
        src/macos/TNRRecorderBridge.mm)
    target_include_directories(minimal_macos_bridge PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/macos)
    target_compile_options(minimal_macos_bridge PRIVATE -fobjc-arc)
    target_link_libraries(minimal_macos_bridge PRIVATE
        tnrp ${APPKIT_FRAMEWORK} ${FOUNDATION_FRAMEWORK})
    set_target_properties(minimal_macos_bridge PROPERTIES
        OUTPUT_NAME TrackNRaceMinimalBridge
        MACOSX_RPATH TRUE
        INSTALL_NAME_DIR "@rpath")

    set(MINIMAL_MACOS_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/macos/MacRecorderApp.swift
        ${CMAKE_CURRENT_SOURCE_DIR}/src/macos/RecorderViewModel.swift
        ${CMAKE_CURRENT_SOURCE_DIR}/src/macos/ContentView.swift
        ${CMAKE_CURRENT_SOURCE_DIR}/src/macos/AttributionsView.swift)
    set(MINIMAL_MACOS_BUNDLE_NAME "Track N Race Minimal Recorder")
    set(MINIMAL_MACOS_APP
        "${CMAKE_CURRENT_BINARY_DIR}/${MINIMAL_MACOS_BUNDLE_NAME}.app")
    set(MINIMAL_MACOS_EXECUTABLE
        "${MINIMAL_MACOS_APP}/Contents/MacOS/${MINIMAL_MACOS_BUNDLE_NAME}")
    set(MINIMAL_MACOS_INFO_PLIST
        "${CMAKE_CURRENT_BINARY_DIR}/generated/Info.plist")
    configure_file(src/macos/Info.plist.in "${MINIMAL_MACOS_INFO_PLIST}" @ONLY)

    add_custom_command(
        OUTPUT "${MINIMAL_MACOS_EXECUTABLE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${MINIMAL_MACOS_APP}/Contents/MacOS"
            "${MINIMAL_MACOS_APP}/Contents/Frameworks"
            "${MINIMAL_MACOS_APP}/Contents/Resources"
            "${CMAKE_CURRENT_BINARY_DIR}/swift-module-cache"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MINIMAL_MACOS_INFO_PLIST}"
            "${MINIMAL_MACOS_APP}/Contents/Info.plist"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/macos/TrackNRace.icns"
            "${MINIMAL_MACOS_APP}/Contents/Resources/TrackNRace.icns"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:minimal_macos_bridge>"
            "${MINIMAL_MACOS_APP}/Contents/Frameworks/$<TARGET_FILE_NAME:minimal_macos_bridge>"
        COMMAND "${MINIMAL_SWIFTC}"
            -parse-as-library
            -O
            -whole-module-optimization
            -sdk "${MINIMAL_MACOS_SDK}"
            -target "${MINIMAL_MACOS_ARCH}-apple-macos${CMAKE_OSX_DEPLOYMENT_TARGET}"
            -module-cache-path "${CMAKE_CURRENT_BINARY_DIR}/swift-module-cache"
            -module-name TrackNRaceMinimalRecorder
            -import-objc-header "${CMAKE_CURRENT_SOURCE_DIR}/src/macos/TNRRecorderBridge.h"
            ${MINIMAL_MACOS_SOURCES}
            -L "$<TARGET_FILE_DIR:minimal_macos_bridge>"
            -lTrackNRaceMinimalBridge
            -framework SwiftUI
            -framework AppKit
            -framework Foundation
            -Xlinker -rpath
            -Xlinker "@executable_path/../Frameworks"
            -o "${MINIMAL_MACOS_EXECUTABLE}"
        DEPENDS
            minimal_macos_bridge
            "$<TARGET_FILE:minimal_macos_bridge>"
            ${MINIMAL_MACOS_SOURCES}
            src/macos/TNRRecorderBridge.h
            "${MINIMAL_MACOS_INFO_PLIST}"
            assets/macos/TrackNRace.icns
        COMMAND_EXPAND_LISTS
        VERBATIM)
    add_custom_target(minimal_app ALL DEPENDS "${MINIMAL_MACOS_EXECUTABLE}")

    install(DIRECTORY "${MINIMAL_MACOS_APP}" DESTINATION .
            USE_SOURCE_PERMISSIONS COMPONENT MinimalApp)
    return()
endif()

if(NOT UNIX)
    message(FATAL_ERROR "minimal_app supports Windows, macOS, and desktop Linux only")
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
