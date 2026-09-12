cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED TNR_APP_VERSION)
    if(DEFINED ENV{TNR_APP_VERSION} AND NOT "$ENV{TNR_APP_VERSION}" STREQUAL "")
        set(TNR_APP_VERSION "$ENV{TNR_APP_VERSION}")
    else()
        set(TNR_APP_VERSION "1.0.0")
    endif()
endif()
project(NativeRecorder VERSION "${TNR_APP_VERSION}" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Default to an optimized build when the user didn't choose one. Only meaningful
# for single-config generators (Make/Ninja); multi-config ones (MSVC, Ninja Multi-
# Config) pick the config at build time via --config. CMake's Release flags already
# imply -O3 -DNDEBUG (GCC/Clang) or /O2 (MSVC) for *every* target, including the
# FetchContent deps below.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Opt-in: tune the binary for the build machine's exact CPU. Faster, but the
# result is NOT portable — running it on an older/different CPU can fault with
# SIGILL. Leave OFF for distributable builds. (No effect under MSVC.)
option(NATIVE_RECORDER_MARCH_NATIVE
    "Tune the recorder for the build machine's CPU (-march=native; not portable)" OFF)

include(FetchContent)

# All row (de)serialization is typed via libtnrp/glaze (tnrp/AnyRow.h); the only
# JSON the app parses itself is the bundled track-map resources, via QJsonDocument.

# TNRD compression is owned entirely by libtnrp: Zstandard for V2/default writes,
# plus zlib for V1/gzip read and deprecated-write compatibility.

# The purpose-built chart renderer uses QRhiWidget (Qt 6.7+) and Qt's shader
# pipeline. QRhi selects Vulkan, Direct3D, Metal or OpenGL at runtime; GuiPrivate
# is intentionally confined to the chart/backend translation units.
find_package(Qt6 6.7 REQUIRED COMPONENTS Core Widgets Network Svg ShaderTools)
find_package(Qt6GuiPrivate REQUIRED)
set(QT_LIBS Qt6::Core Qt6::Widgets Qt6::Network Qt6::Svg Qt6::GuiPrivate)
message(STATUS "Using Qt6 QRhi telemetry renderer")

# Shared telemetry engine (libtnrp / target `tnrp`): UDP receive, F1 24/25 packet
# parsing, .tnrd recording + playback — the single source of truth also used by the
# Electron node addon. TNRP_USE_QT=ON selects its QUdpSocket backend so it integrates
# with this app's Qt build (no raw sockets). The guard lets a parent superbuild add
# the library first. The library fetches/links its codecs and glaze itself.
set(TNRP_USE_QT ON CACHE BOOL "" FORCE)
if(NOT TARGET tnrp)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../protocol_parser_library
                     ${CMAKE_BINARY_DIR}/tnrp_build)
endif()

set(SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/AppToolbar.cpp
    src/AppToolbar.h
    src/PlaybackController.cpp
    src/PlaybackController.h
    src/EngineSink.h
    src/HotRowSmoother.h
    src/PresentationScheduler.h
    src/CompactSettings.h
    src/GraphViewSettings.h
    src/BreezePalette.cpp
    src/BreezePalette.h
    src/TnrdPlayer.cpp
    src/TnrdPlayer.h
    src/SessionModel.cpp
    src/ChartCoordinates.cpp
    src/SessionModel.h
    src/ChartGraphicsBackend.cpp
    src/ChartGraphicsBackend.h
    src/TelemetryChart.cpp
    src/TelemetryChart.h
    src/components/ChartView.cpp
    src/components/ChartView.h
    src/components/GraphTable.cpp
    src/components/GraphTable.h
    src/components/PageUiHelpers.h
    src/components/Toast.cpp
    src/components/Toast.h
    src/components/XlsxExportWorker.cpp
    src/components/XlsxExportWorker.h
    src/components/ToastEnums.h
    src/components/ToastEvents.cpp
    src/components/ToastEvents.h
    src/components/ToastHost.h
    src/components/ToastHost.cpp
    src/components/OverviewLayout.h
    src/components/OverviewLayout.cpp
    src/components/EditOverviewLayoutDialog.h
    src/components/EditOverviewLayoutDialog.cpp
    src/components/EditInputLayoutDialog.h
    src/components/EditInputLayoutDialog.cpp
    src/components/EditPowerLayoutDialog.h
    src/components/EditPowerLayoutDialog.cpp
    src/components/OverviewPage.h
    src/components/OverviewPage.cpp
    src/components/AnalyzeMetrics.h
    src/components/AnalyzeMetrics.cpp
    src/components/AnalyzeChart.h
    src/components/AnalyzeChart.cpp
    src/components/AnalyzePage.h
    src/components/AnalyzePage.cpp
    src/components/SettingsDialog.h
    src/components/SettingsDialog.cpp
    src/components/TyresPage.h
    src/components/TyresPage.cpp
    src/components/TyreCardsWidget.h
    src/components/TyreCardsWidget.cpp
    src/components/TyreChartsWidget.h
    src/components/TyreChartsWidget.cpp
    src/components/StrategyPage.h
    src/components/StrategyPage.cpp
    src/components/StandingsPage.h
    src/components/StandingsPage.cpp
    src/components/SessionPage.h
    src/components/SessionPage.cpp
    src/components/InputPage.h
    src/components/InputPage.cpp
    src/components/PowerPage.h
    src/components/PowerPage.cpp
    src/components/MiscPage.h
    src/components/MiscPage.cpp
    src/components/EditMiscLayoutDialog.h
    src/components/EditMiscLayoutDialog.cpp
    src/components/EditSessionLayoutDialog.h
    src/components/EditSessionLayoutDialog.cpp
    src/components/PowerChartsWidget.cpp
    src/components/PowerChartsWidget.h
    src/components/InputChartsWidget.cpp
    src/components/InputChartsWidget.h
    src/components/MiscChartsWidget.cpp
    src/components/MiscChartsWidget.h
    src/components/TrackMapWidget.cpp
    src/components/TrackMapWidget.h
    resources.qrc
    maps.qrc
    licenses.qrc
    fonts.qrc
)

if(WIN32)
    list(APPEND SOURCES resources.rc)
    add_executable(native_recorder WIN32 ${SOURCES})
    target_compile_options(native_recorder PRIVATE /utf-8 /FIwindows.h)
    # NOMINMAX prevents windows.h min/max macros from shadowing std::min/std::max.
    # The /FI force-include ensures tagMSG is fully defined before Qt forward-declares
    # it, so type-trait-heavy templates compile cleanly under MSVC.
    target_compile_definitions(native_recorder PRIVATE NOMINMAX)
else()
    add_executable(native_recorder ${SOURCES})
endif()

set_target_properties(native_recorder PROPERTIES OUTPUT_NAME "Track-N-Race - Qt")
# Single source of truth for the version shown in Settings → About (read at
# runtime via QApplication::applicationVersion()).
target_compile_definitions(native_recorder PRIVATE APP_VERSION="${PROJECT_VERSION}")
target_link_libraries(native_recorder PRIVATE tnrp ${QT_LIBS})

qt_add_shaders(native_recorder "tnr_chart_shaders"
    PREFIX "/shaders"
    BASE "shaders"
    FILES
        shaders/chart.vert
        shaders/chart.frag
)

# Keep every directly-linked Qt module in DT_NEEDED even if whole-program LTO
# proves none of its symbols are reachable (e.g. QSvgRenderer/QPrinter/
# QOpenGLWidget calls fully inlined away). Some toolchains' --as-needed default
# is aggressive enough, combined with IPO, to drop libQt6Svg/PrintSupport/
# OpenGLWidgets from NEEDED entirely — which then makes linuxdeploy-plugin-qt
# (which decides what Qt plugins to bundle by scanning NEEDED) skip the SVG
# icon engine and print-support plugins, silently breaking Breeze's SVG icons
# and GPU chart rendering in the packaged AppImage despite an identical build
# on another machine/toolchain working fine.
if(UNIX AND NOT APPLE)
    target_link_options(native_recorder PRIVATE LINKER:--no-as-needed)
endif()

# ── Maximum compiler optimization (applied to optimized configs only) ─────────
# Link-time optimization (whole-program / cross-TU inlining) where the toolchain
# supports it. Enabled per-config so Debug builds stay fast and debuggable.
include(CheckIPOSupported)
check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_msg LANGUAGES CXX)
if(_ipo_ok)
    set_target_properties(native_recorder PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION_RELEASE        ON
        INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON
        INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL     ON)
    message(STATUS "LTO/IPO enabled for optimized builds")
else()
    message(STATUS "LTO/IPO not supported by this toolchain: ${_ipo_msg}")
endif()

if(MSVC)
    # /O2 full speed, /Oi intrinsics, /Ot favour speed. /GL+/LTCG is the IPO path
    # CMake already wires up above, so it isn't repeated here. /OPT:REF,/OPT:ICF
    # strip unused code and fold identical functions at link.
    target_compile_options(native_recorder PRIVATE
        $<$<NOT:$<CONFIG:Debug>>:/O2 /Oi /Ot>)
    target_link_options(native_recorder PRIVATE
        $<$<NOT:$<CONFIG:Debug>>:/OPT:REF /OPT:ICF>)
else()
    # -O3 is already in CMake's Release/RelWithDebInfo flags, but state it
    # explicitly so any custom build type still gets it. -fno-math-errno is the
    # one "fast-math" relaxation that's safe here (we never read errno after
    # math calls); full -ffast-math is intentionally avoided to keep telemetry
    # numbers IEEE-correct.
    target_compile_options(native_recorder PRIVATE
        $<$<NOT:$<CONFIG:Debug>>:-O3 -fno-math-errno>)
    if(NATIVE_RECORDER_MARCH_NATIVE)
        target_compile_options(native_recorder PRIVATE
            $<$<NOT:$<CONFIG:Debug>>:-march=native>)
        message(STATUS "Building with -march=native (binary not portable to other CPUs)")
    endif()
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
    # Marks a build that bundles the Breeze/KF6 stack (i.e. build.sh --with-breeze).
    # The About dialog lists the bundled KDE components only when this is defined.
    target_compile_definitions(native_recorder PRIVATE BREEZE_BUNDLED)

    set(_breeze_styldir "$<TARGET_FILE_DIR:native_recorder>/plugins/styles")
    set(_breeze_appdir  "$<TARGET_FILE_DIR:native_recorder>")

    # Link KColorScheme (already in the bundle) so the app can build the exact KDE
    # Breeze palette at runtime — the Breeze plugin only draws shapes, the colours
    # come from the QPalette, which off-KDE we must set ourselves (see
    # BreezePalette.cpp). Found via the superbuild prefix.
    list(APPEND CMAKE_PREFIX_PATH "${BREEZE_STACK_PREFIX}")
    find_package(KF6ColorScheme QUIET)
    if(KF6ColorScheme_FOUND)
        target_link_libraries(native_recorder PRIVATE KF6::ColorScheme KF6::ConfigCore)
        target_compile_definitions(native_recorder PRIVATE HAVE_BREEZE_KCOLORSCHEME)
    else()
        message(WARNING "KF6ColorScheme not found under ${BREEZE_STACK_PREFIX} — Breeze will "
                        "load but render with the host's default palette, not KDE colours.")
    endif()

    # Link KF6BreezeIcons (already in the bundle) so we can call BreezeIcons::
    # initIcons() — it registers the embedded Breeze icon theme under :/icons and
    # makes it QIcon's fallback (Linux: OS theme first; Windows: we set it primary).
    find_package(KF6BreezeIcons QUIET)
    if(KF6BreezeIcons_FOUND)
        target_link_libraries(native_recorder PRIVATE KF6::BreezeIcons)
        target_compile_definitions(native_recorder PRIVATE HAVE_BREEZE_ICONS)
    else()
        message(WARNING "KF6BreezeIcons not found under ${BREEZE_STACK_PREFIX} — the bundled "
                        "Breeze icon theme won't be set up.")
    endif()

    # Bundle the Breeze colour-scheme files (installed by the superbuild) next to
    # the app; BreezePalette.cpp loads them from <appdir>/color-schemes/. KDE's
    # data dir is share/ on Unix but bin/data/ on Windows (KDE_INSTALL_USE_QT_SYS_
    # PATHS=OFF), so look in both.
    file(GLOB _breeze_colors
        "${BREEZE_STACK_PREFIX}/share/color-schemes/Breeze*.colors"
        "${BREEZE_STACK_PREFIX}/bin/data/color-schemes/Breeze*.colors")
    if(_breeze_colors)
        add_custom_command(TARGET native_recorder POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_breeze_appdir}/color-schemes"
            COMMAND ${CMAKE_COMMAND} -E copy ${_breeze_colors} "${_breeze_appdir}/color-schemes/"
            COMMENT "Bundling Breeze colour schemes"
            VERBATIM)
    else()
        message(WARNING "No Breeze*.colors under ${BREEZE_STACK_PREFIX}/share/color-schemes — "
                        "the KDE palette can't be applied; Breeze will use a fallback palette.")
    endif()

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

        # The app itself now links KF6ColorScheme, so its binary must find the
        # bundled KF6 libs in lib_breeze/ at startup — add (not replace) the RPATH.
        if(KF6ColorScheme_FOUND)
            add_custom_command(TARGET native_recorder POST_BUILD
                COMMAND ${PATCHELF_EXE} --add-rpath "$ORIGIN/lib_breeze" "$<TARGET_FILE:native_recorder>"
                VERBATIM)
        endif()
    endif()
endif()
