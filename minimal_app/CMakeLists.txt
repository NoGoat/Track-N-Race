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
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/../native_recorder/licenses/LGPL-2.1.txt LGPL21_TEXT)
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

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4>=4.6)
find_program(GLIB_COMPILE_SCHEMAS glib-compile-schemas REQUIRED)

set(SCHEMA_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/schemas)
file(MAKE_DIRECTORY ${SCHEMA_BUILD_DIR})
configure_file(data/in.nogoat.tracknrace.gschema.xml
               ${SCHEMA_BUILD_DIR}/in.nogoat.tracknrace.gschema.xml COPYONLY)
add_custom_command(
    OUTPUT ${SCHEMA_BUILD_DIR}/gschemas.compiled
    COMMAND ${GLIB_COMPILE_SCHEMAS} --strict ${SCHEMA_BUILD_DIR}
    DEPENDS ${SCHEMA_BUILD_DIR}/in.nogoat.tracknrace.gschema.xml
    COMMENT "Compiling minimal recorder GSettings schema")
add_custom_target(minimal_app_schemas ALL DEPENDS ${SCHEMA_BUILD_DIR}/gschemas.compiled)

add_executable(minimal_app ${COMMON_SOURCES} src/linux/gtk_main.cpp)
add_dependencies(minimal_app minimal_app_schemas)
target_link_libraries(minimal_app PRIVATE tnrp PkgConfig::GTK4)
target_compile_definitions(minimal_app PRIVATE TNR_MINIMAL_GTK)
set_target_properties(minimal_app PROPERTIES OUTPUT_NAME track-n-race-minimal)

target_include_directories(minimal_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

include(GNUInstallDirs)
install(TARGETS minimal_app RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/in.nogoat.tracknrace.gschema.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/glib-2.0/schemas)
install(FILES data/track-n-race-minimal.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../icon_transparent.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps
        RENAME track-n-race-minimal.png)
install(CODE "
    set(_schema_dir \"\$ENV{DESTDIR}${CMAKE_INSTALL_FULL_DATADIR}/glib-2.0/schemas\")
    if(EXISTS \"\${_schema_dir}\")
        execute_process(COMMAND \"${GLIB_COMPILE_SCHEMAS}\" \"\${_schema_dir}\"
                        RESULT_VARIABLE _schema_result)
        if(NOT _schema_result EQUAL 0)
            message(FATAL_ERROR \"glib-compile-schemas failed during install\")
        endif()
    endif()
")
