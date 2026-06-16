# ─────────────────────────────────────────────────────────────────────────────
# Breeze style superbuild
#
# Builds ONLY the KDE Breeze Qt6 *widget style* plugin (breeze6) plus the minimal
# set of KDE Frameworks it actually links — no Qt Quick, no Kirigami, no window
# decorations, no KCM. The result is an install prefix you can bundle next to the
# app (plugins/styles/breeze6.* + the KF6 runtime libs). The app itself does NOT
# link any of this; Breeze is loaded at runtime via QStyleFactory.
#
# This is a *separate* project from the app on purpose: each KF6 framework does
# find_package(KF6Dep) at configure time, so they must be built+installed in
# dependency order into a shared prefix (classic ExternalProject superbuild),
# which a single FetchContent_MakeAvailable tree cannot do.
#
# ── Usage ────────────────────────────────────────────────────────────────────
#   cmake -S breeze_stack -B build/breeze_stack \
#         -DCMAKE_BUILD_TYPE=Release \
#         -DBREEZE_QT_PREFIX=/usr            # <- the SAME Qt the app links!
#   cmake --build build/breeze_stack -j
#
# Artifacts land in:  build/breeze_stack/prefix/
#   prefix/lib/qt6/plugins/styles/breeze6.so   (or .dll/.dylib)
#   prefix/lib/libKF6*.so ...                  (Breeze's runtime deps)
#
# CRITICAL: BREEZE_QT_PREFIX must point at the exact Qt the app is built against
# (same version + compiler/ABI), or Qt will refuse to load the plugin.
# ─────────────────────────────────────────────────────────────────────────────
cmake_minimum_required(VERSION 3.19)
project(breeze_stack NONE)

include(ExternalProject)

# ── Versions (override on the command line if a tag needs bumping) ────────────
# KF6 frameworks share one release train; keep them on a single tag. Pick a
# release contemporaneous with the app's Qt — a stale KF6 vs a newer Qt fails to
# compile (e.g. QFlags no longer implicitly converting in QString::arg()).
set(KF6_VERSION "6.27.0" CACHE STRING "KDE Frameworks 6 release tag (without leading v)")
# Breeze is released with Plasma, not Frameworks, so it has its own tag.
set(BREEZE_TAG  "v6.7.0" CACHE STRING "Breeze (Plasma) git tag")

# Qt prefix the whole stack builds against — MUST match the app's Qt.
set(BREEZE_QT_PREFIX "" CACHE PATH "Prefix of the Qt6 the app links against (e.g. /usr)")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(PREFIX "${CMAKE_BINARY_DIR}/prefix")

# Prefix path every sub-build sees: our growing install tree first, then Qt.
# Joined with "|" (not ";") because ExternalProject splits ";" in CMAKE_ARGS into
# separate arguments — LIST_SEPARATOR below turns the "|" back into ";" for the
# sub-cmake. Without this the Qt prefix is silently dropped ("Ignoring extra path").
set(STACK_PREFIX_PATH "${PREFIX}")
if(BREEZE_QT_PREFIX)
    set(STACK_PREFIX_PATH "${PREFIX}|${BREEZE_QT_PREFIX}")
endif()

# Args common to every framework/Breeze configure step.
set(COMMON_CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=${PREFIX}
    -DCMAKE_PREFIX_PATH=${STACK_PREFIX_PATH}
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DBUILD_SHARED_LIBS=ON
    -DBUILD_TESTING=OFF
    -DBUILD_EXAMPLES=OFF
    # KF6 frameworks now ship optional Python (PySide/Shiboken6) bindings that
    # configure-fail without Shiboken6 — we don't want bindings at all.
    -DBUILD_PYTHON_BINDINGS=OFF
    # Install everything under our prefix rather than Qt's system paths.
    -DKDE_INSTALL_USE_QT_SYS_PATHS=OFF
)

set(KDE_GIT https://github.com/KDE)

# Helper: declare a KDE framework ExternalProject named <name> from repo <repo>,
# pinned to the KF6 tag, depending on the targets passed after DEPENDS.
function(add_kf name repo)
    cmake_parse_arguments(KF "" "" "DEPENDS;EXTRA" ${ARGN})
    ExternalProject_Add(${name}
        GIT_REPOSITORY ${KDE_GIT}/${repo}.git
        GIT_TAG        v${KF6_VERSION}
        GIT_SHALLOW    ON
        LIST_SEPARATOR "|"
        CMAKE_ARGS     ${COMMON_CMAKE_ARGS} ${KF_EXTRA}
        DEPENDS        ${KF_DEPENDS}
        # No update step: we pin a tag (detached HEAD), so a git-pull would only
        # error with "HEAD does not point to a branch".
        UPDATE_COMMAND ""
    )
endfunction()

# ── Build-time tooling ───────────────────────────────────────────────────────
add_kf(ecm extra-cmake-modules)

# ── Tier-1 leaf frameworks (depend only on ECM + Qt) ─────────────────────────
add_kf(kcoreaddons   kcoreaddons   DEPENDS ecm)
add_kf(kconfig       kconfig       DEPENDS ecm)
add_kf(kcodecs       kcodecs       DEPENDS ecm)
# Breeze uses KGuiAddons only for colour utilities — drop its Wayland backend
# (KModifierKeyInfo/clipboard) so we don't need wayland-protocols et al.
add_kf(kguiaddons    kguiaddons    DEPENDS ecm EXTRA -DWITH_WAYLAND=OFF)
add_kf(kwidgetsaddons kwidgetsaddons DEPENDS ecm)
# Disable both KWindowSystem platform backends: they only require XCB/Wayland
# protocol dev packages for desktop integration (blur-behind etc., cosmetic),
# which Breeze degrades gracefully without. Auto-off on Windows/macOS anyway.
add_kf(kwindowsystem kwindowsystem DEPENDS ecm
       EXTRA -DKWINDOWSYSTEM_X11=OFF -DKWINDOWSYSTEM_WAYLAND=OFF)
# KI18n needs gettext/libintl — present on Linux/macOS; the sticky bit on MSVC
# (see the plan's Risks). Build minimal.
add_kf(ki18n         ki18n         DEPENDS ecm)
# KArchive only needs zlib for Breeze's purposes — drop the other backends.
add_kf(karchive      karchive      DEPENDS ecm
       EXTRA -DWITH_BZIP2=OFF -DWITH_LIBLZMA=OFF -DWITH_LIBZSTD=OFF)
# breeze-icons: KIconThemes hard-links libKF6BreezeIcons (a compiled-in fallback
# icon theme), so we must build it here or the bundle silently resolves it from
# the system /usr — which won't exist on a non-KDE/Windows target.
add_kf(breezeicons   breeze-icons  DEPENDS ecm)

# ── Tier-2 (depend on the leaves above) ──────────────────────────────────────
add_kf(kcolorscheme  kcolorscheme  DEPENDS ecm kconfig kguiaddons kcoreaddons)
add_kf(kconfigwidgets kconfigwidgets DEPENDS ecm kconfig kcodecs kwidgetsaddons
       kguiaddons kcoreaddons kcolorscheme ki18n)
add_kf(kiconthemes   kiconthemes   DEPENDS ecm kconfigwidgets karchive kcolorscheme
       kwidgetsaddons kconfig kcoreaddons breezeicons)

# ── Breeze widget style only — every heavy option OFF ────────────────────────
# Links: Qt6 Core/Gui/Widgets/Svg + KF6 CoreAddons/Config/GuiAddons/IconThemes/
# WindowSystem/ColorScheme (FrameworkIntegration/KStyle is optional and skipped).
ExternalProject_Add(breeze
    GIT_REPOSITORY ${KDE_GIT}/breeze.git
    GIT_TAG        ${BREEZE_TAG}
    GIT_SHALLOW    ON
    LIST_SEPARATOR "|"
    CMAKE_ARGS     ${COMMON_CMAKE_ARGS}
                   -DBUILD_QT5=OFF
                   -DBUILD_QT6=ON
                   -DBUILD_WITH_QTQUICK=OFF
                   -DWITH_DECORATIONS=OFF
                   -DWITH_WALLPAPERS=OFF
                   -DBUILD_CURSOR=OFF
                   # Don't link KStyle (FrameworkIntegration): Breeze guards it
                   # (if FOUND) and works as a plain QStyle without it, so this
                   # drops the libKF6Style runtime dep instead of building yet
                   # another framework. Also skip the style-config KCM (KCMUtils),
                   # which we never bundle and which would want KStyle.
                   -DCMAKE_DISABLE_FIND_PACKAGE_KF6FrameworkIntegration=ON
                   -DCMAKE_DISABLE_FIND_PACKAGE_KF6KCMUtils=ON
    DEPENDS        ecm kcoreaddons kconfig kguiaddons kwindowsystem
                   kcolorscheme kiconthemes breezeicons
    UPDATE_COMMAND ""
)

# Where the built style plugin ends up (consumed by the app's bundling step).
message(STATUS "Breeze stack install prefix: ${PREFIX}")
message(STATUS "  style plugin -> ${PREFIX}/lib/qt6/plugins/styles/")
message(STATUS "  Qt prefix used: ${BREEZE_QT_PREFIX}")
