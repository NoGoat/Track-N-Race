cmake_minimum_required(VERSION 3.16)
project(NativeRecorder CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

include(FetchContent)

# Fetch nlohmann/json for JSON serialization
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

# Fetch zlib for .tnrd gzip streaming
FetchContent_Declare(
    zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG v1.3.1
)
FetchContent_MakeAvailable(zlib)

# Find Qt6 (fallback to Qt5). PrintSupport is required by the vendored QCustomPlot.
# OpenGL/OpenGLWidgets let QCustomPlot rasterize on the GPU — software QPainter
# rendering of the Speed/RPM/ERS chart costs ~100 ms/frame (and seconds/frame at
# a 10-min window), which GPU rendering cuts to single-digit ms. See setOpenGl().
find_package(Qt6 QUIET COMPONENTS Core Widgets Network Svg PrintSupport OpenGL OpenGLWidgets)
if(Qt6_FOUND)
    set(QT_LIBS Qt6::Core Qt6::Widgets Qt6::Network Qt6::Svg Qt6::PrintSupport
                Qt6::OpenGL Qt6::OpenGLWidgets)
    set(QCP_OPENGL ON)
    message(STATUS "Using Qt6 (QCustomPlot OpenGL rendering enabled)")
else()
    find_package(Qt5 REQUIRED COMPONENTS Core Widgets Network Svg PrintSupport)
    set(QT_LIBS Qt5::Core Qt5::Widgets Qt5::Network Qt5::Svg Qt5::PrintSupport)
    message(STATUS "Using Qt5 (Qt6 not found; software chart rendering)")
endif()

set(SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/TnrdPlayer.cpp
    src/TnrdPlayer.h
    src/SessionModel.cpp
    src/SessionModel.h
    src/TelemetryChart.cpp
    src/TelemetryChart.h
    src/components/ChartView.cpp
    src/components/ChartView.h
    src/third_party/qcustomplot/qcustomplot.cpp
    src/third_party/qcustomplot/qcustomplot.h
    src/components/OverviewLayout.h
    src/components/OverviewLayout.cpp
    src/components/EditOverviewLayoutDialog.h
    src/components/EditOverviewLayoutDialog.cpp
    src/components/OverviewPage.cpp
    src/components/SettingsDialog.h
    src/components/SettingsDialog.cpp
    src/components/TyresPage.cpp
    src/components/StandingsPage.cpp
    src/components/SessionPage.cpp
    src/components/TrackMapPage.cpp
    src/components/TrackMapWidget.cpp
    src/components/TrackMapWidget.h
    src/protocols/protocol.cpp
    src/protocols/f1_24.cpp
    src/protocols/f1_25.cpp
    resources.qrc
    maps.qrc
)

if(WIN32)
    list(APPEND SOURCES resources.rc)
    add_executable(native_recorder WIN32 ${SOURCES})
    target_compile_options(native_recorder PRIVATE /utf-8 /FI"windows.h")
    # NOMINMAX prevents windows.h min/max macros from shadowing std::min/std::max.
    # The /FI force-include ensures tagMSG is fully defined before Qt forward-declares
    # it, so nlohmann::json's type-trait templates compile cleanly under MSVC.
    target_compile_definitions(native_recorder PRIVATE NOMINMAX)
else()
    add_executable(native_recorder ${SOURCES})
endif()

set_target_properties(native_recorder PROPERTIES OUTPUT_NAME "Track N Race Background Recorder")
target_link_libraries(native_recorder PRIVATE nlohmann_json::nlohmann_json zlibstatic ${QT_LIBS})

# GPU rendering for QCustomPlot (Qt6 only). The macro must reach qcustomplot.cpp,
# so define it on the whole target. OpenGL::GL provides the GL symbols the
# QCustomPlot OpenGL paint path references directly.
if(QCP_OPENGL)
    find_package(OpenGL REQUIRED)
    target_compile_definitions(native_recorder PRIVATE QCUSTOMPLOT_USE_OPENGL)
    target_link_libraries(native_recorder PRIVATE OpenGL::GL)
endif()

# ── Optional: bundle a prebuilt Breeze style stack (see breeze_stack/) ────────
# When BREEZE_STACK_PREFIX points at the superbuild's install prefix, copy the
# Breeze style plugin + its exact KF6 runtime closure next to the app so Breeze
# is selectable on machines without KDE. The app never links any of this; Qt
# loads breeze6.so at runtime via QStyleFactory (main.cpp adds <exe>/plugins to
# the library path), and breeze6.so pulls the KF6 libs in via an $ORIGIN RPATH.
# Leaving BREEZE_STACK_PREFIX unset (the default) makes this a no-op, so normal
# dev builds are unaffected.
set(BREEZE_STACK_PREFIX "" CACHE PATH "Install prefix from the breeze_stack/ superbuild; set to bundle Breeze next to the app")
# Windows only: directory holding the non-KF6 runtime DLLs the stack links but
# which live outside the superbuild prefix — libintl (intl-8.dll, used by
# KF6I18n) and its iconv companion. Point at the gettext bin, e.g. the vcpkg
# install's  <vcpkg>/installed/x64-windows/bin .
set(BREEZE_WIN_RUNTIME_DIR "" CACHE PATH "Windows: dir with intl-8.dll/iconv (e.g. vcpkg .../x64-windows/bin)")

if(BREEZE_STACK_PREFIX)
    set(_breeze_styldir "$<TARGET_FILE_DIR:native_recorder>/plugins/styles")
    set(_breeze_appdir  "$<TARGET_FILE_DIR:native_recorder>")

    if(WIN32)
        # Windows resolves a plugin's dependent DLLs from the *executable's*
        # directory, so the KF6 DLLs sit next to the .exe and breeze6.dll goes in
        # plugins/styles/. No RPATH/patchelf concept here — just copies.
        # Glob all KF6 + zlib DLLs from the prefix (a superset of breeze6.dll's
        # closure; the unused ones are never loaded, so they're harmless). Globbed
        # at configure time — reconfigure if the superbuild's DLL set changes.
        file(GLOB _breeze_win_dlls "${BREEZE_STACK_PREFIX}/bin/*.dll")

        add_custom_command(TARGET native_recorder POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_breeze_styldir}"
            COMMAND ${CMAKE_COMMAND} -E copy
                    "${BREEZE_STACK_PREFIX}/lib/plugins/styles/breeze6.dll" "${_breeze_styldir}/"
            COMMAND ${CMAKE_COMMAND} -E copy ${_breeze_win_dlls} "${_breeze_appdir}/"
            COMMENT "Bundling Breeze style plugin + KF6 DLLs (Windows)"
            VERBATIM)

        # libintl/iconv (KF6I18n's runtime deps) live in the gettext install, not
        # the prefix — copy them next to the .exe too.
        if(BREEZE_WIN_RUNTIME_DIR)
            file(GLOB _breeze_rt_dlls
                "${BREEZE_WIN_RUNTIME_DIR}/intl-*.dll"   "${BREEZE_WIN_RUNTIME_DIR}/libintl*.dll"
                "${BREEZE_WIN_RUNTIME_DIR}/iconv*.dll"   "${BREEZE_WIN_RUNTIME_DIR}/libiconv*.dll"
                "${BREEZE_WIN_RUNTIME_DIR}/charset*.dll" "${BREEZE_WIN_RUNTIME_DIR}/libcharset*.dll")
            if(_breeze_rt_dlls)
                add_custom_command(TARGET native_recorder POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy ${_breeze_rt_dlls} "${_breeze_appdir}/"
                    VERBATIM)
            endif()
        else()
            message(WARNING "BREEZE_WIN_RUNTIME_DIR not set — intl-8.dll/iconv (needed by KF6I18n) "
                            "won't be bundled, so Breeze will fail to load. Set it to your gettext "
                            "bin, e.g. C:/vcpkg/installed/x64-windows/bin.")
        endif()

    elseif(APPLE)
        message(WARNING "Breeze bundling for macOS not implemented yet; skipping.")

    elseif(UNIX)
        # patchelf rewrites the absolute build-prefix RPATHs to $ORIGIN-relative
        # ones so the copied libs find each other on any machine.
        find_program(PATCHELF_EXE patchelf)
        if(NOT PATCHELF_EXE)
            message(FATAL_ERROR "patchelf not found — required to set \$ORIGIN RPATH on the "
                                "bundled Breeze libs. Install it (e.g. pacman -S patchelf).")
        endif()

        # Exact runtime closure of breeze6.so, verified via `ldd` (sonames only —
        # each is a symlink to the real .so.6.x.y; cmake -E copy dereferences it).
        # If the Breeze/KF6 versions are bumped, re-verify this list with ldd.
        set(_breeze_kf6_libs
            libKF6CoreAddons.so.6  libKF6IconThemes.so.6  libKF6WindowSystem.so.6
            libKF6ColorScheme.so.6 libKF6ConfigGui.so.6   libKF6ConfigCore.so.6
            libKF6GuiAddons.so.6   libKF6Archive.so.6     libKF6BreezeIcons.so.6
            libKF6I18n.so.6)

        set(_breeze_libdir "$<TARGET_FILE_DIR:native_recorder>/lib_breeze")

        # Style plugin → plugins/styles/, RPATH pointing two levels up to lib_breeze/.
        add_custom_command(TARGET native_recorder POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_breeze_libdir}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_breeze_styldir}"
            COMMAND ${CMAKE_COMMAND} -E copy
                    "${BREEZE_STACK_PREFIX}/lib/plugins/styles/breeze6.so" "${_breeze_styldir}/"
            COMMAND ${PATCHELF_EXE} --set-rpath "$ORIGIN/../../lib_breeze" "${_breeze_styldir}/breeze6.so"
            COMMENT "Bundling Breeze style plugin + KF6 runtime closure"
            VERBATIM)

        # Each KF6 lib → lib_breeze/ (real content under its soname), RPATH=$ORIGIN
        # so it finds its siblings in the same directory.
        foreach(_lib ${_breeze_kf6_libs})
            add_custom_command(TARGET native_recorder POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy
                        "${BREEZE_STACK_PREFIX}/lib/${_lib}" "${_breeze_libdir}/"
                COMMAND ${PATCHELF_EXE} --set-rpath "$ORIGIN" "${_breeze_libdir}/${_lib}"
                VERBATIM)
        endforeach()
    endif()
endif()
