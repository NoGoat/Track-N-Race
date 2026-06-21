#!/usr/bin/env bash
# build-appimage.sh — Build the Track N Race Background Recorder into a
# self-contained AppImage on Linux, with the Breeze style stack bundled.
#
# This reuses build.sh --with-breeze, which already compiles the app and stages
# the full Breeze runtime next to the executable in build/ (lib_breeze/,
# plugins/styles/breeze6.so, color-schemes/ — all with $ORIGIN-relative RPATHs).
# We just copy that proven bundle into an AppDir and let linuxdeploy add the Qt
# runtime and pack the AppImage. Because the breeze layout is already relative to
# the executable's directory, it survives the move into AppDir/usr/bin unchanged.
#
# Usage: ./build-appimage.sh [--qt-prefix /path/to/Qt/6.x.x/gcc_64]
#                            [--breeze-prefix /path/to/breeze_stack/prefix]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
APPDIR="$SCRIPT_DIR/AppDir"
TOOLS_DIR="$SCRIPT_DIR/.appimage-tools"

QT_PREFIX="${QT_PREFIX:-}"
BREEZE_PREFIX=""   # empty => build.sh's default ($BUILD_DIR/breeze_stack/prefix)
while [[ $# -gt 0 ]]; do
    case "$1" in
        --qt-prefix)     QT_PREFIX="$2"; shift 2 ;;
        --breeze-prefix) BREEZE_PREFIX="$2"; shift 2 ;;
        *) echo "Unknown argument: $1"; exit 1 ;;
    esac
done

echo "============================================="
echo "   TNRD Background Recorder - AppImage Build"
echo "============================================="

# ── 1. Build the app + Breeze bundle via the existing build script ───────────
BUILD_ARGS=(--with-breeze)
[[ -n "$BREEZE_PREFIX" ]] && BUILD_ARGS+=("$BREEZE_PREFIX")
[[ -n "$QT_PREFIX" ]] && BUILD_ARGS+=(--qt-prefix "$QT_PREFIX")

echo ""
echo "[1/4] Building (build.sh ${BUILD_ARGS[*]})..."
"$SCRIPT_DIR/build.sh" "${BUILD_ARGS[@]}"

BIN="$BUILD_DIR/Track N Race Background Recorder"
if [[ ! -f "$BIN" ]]; then
    echo "ERROR: built binary not found at: $BIN" >&2
    exit 1
fi
if [[ ! -d "$BUILD_DIR/lib_breeze" ]]; then
    echo "ERROR: Breeze bundle (lib_breeze/) missing — did --with-breeze run?" >&2
    exit 1
fi

# ── 2. Stage the AppDir ──────────────────────────────────────────────────────
# build/ is a 3.3 GB CMake build dir (CMakeFiles/, _deps/, test binaries …), so
# it can't be the AppDir itself — appimagetool packs an AppDir verbatim, and
# symlinks back into build/ break once the image is mounted. So we lift only the
# app payload out of build/ into a clean AppDir: the binary plus the three Breeze
# sibling dirs it needs. AppImage tooling chokes on the spaces in the binary's
# OUTPUT_NAME, so it's renamed to track-n-race-recorder (matching the .desktop
# Exec=/Icon=). Breeze's RPATHs are relative to the executable's directory, so
# lib_breeze/ and plugins/ must sit right next to the renamed binary in usr/bin —
# exactly as they sit next to it in build/.
echo ""
echo "[2/4] Staging AppDir..."
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" \
         "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/icons/hicolor/256x256/apps"

cp "$BIN" "$APPDIR/usr/bin/track-n-race-recorder"
cp -r "$BUILD_DIR/lib_breeze" "$APPDIR/usr/bin/"
cp -r "$BUILD_DIR/plugins"    "$APPDIR/usr/bin/"
[[ -d "$BUILD_DIR/color-schemes" ]] && cp -r "$BUILD_DIR/color-schemes" "$APPDIR/usr/bin/"

cp "$SCRIPT_DIR/assets/linux/track-n-race-recorder.desktop" \
   "$APPDIR/usr/share/applications/"
cp "$SCRIPT_DIR/assets/linux/track-n-race-recorder.png" \
   "$APPDIR/usr/share/icons/hicolor/256x256/apps/"

# ── 3. qmake for linuxdeploy's Qt plugin ─────────────────────────────────────
if [[ -n "$QT_PREFIX" ]]; then
    QMAKE="$QT_PREFIX/bin/qmake"
else
    QMAKE="$(command -v qmake6 || command -v qmake || true)"
fi
if [[ -z "$QMAKE" || ! -x "$QMAKE" ]]; then
    echo "ERROR: qmake not found. Install Qt6 dev tools or pass --qt-prefix." >&2
    exit 1
fi
export QMAKE
echo "Using qmake: $QMAKE"

# ── 4. Fetch linuxdeploy + Qt plugin (cached) and pack ───────────────────────
mkdir -p "$TOOLS_DIR"
LD="$TOOLS_DIR/linuxdeploy-x86_64.AppImage"
LDQT="$TOOLS_DIR/linuxdeploy-plugin-qt-x86_64.AppImage"
APPIMAGETOOL="$TOOLS_DIR/appimagetool-x86_64.AppImage"
fetch() { # url dest
    [[ -f "$2" ]] && return 0
    echo "Downloading $(basename "$2")..."
    curl -fL --retry 3 -o "$2" "$1"
    chmod +x "$2"
}
fetch "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage" "$LD"
fetch "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage" "$LDQT"
fetch "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage" "$APPIMAGETOOL"

# extract-and-run avoids needing FUSE on the build host; plugins are found on PATH.
export APPIMAGE_EXTRACT_AND_RUN=1
export PATH="$TOOLS_DIR:$PATH"
export ARCH=x86_64
# Don't strip: linuxdeploy bundles an old binutils strip that can't parse the
# .relr.dyn (DT_RELR) sections this host's modern toolchain emits, which makes
# every strip call fail and aborts the run. Skipping it just leaves libs unstripped.
export NO_STRIP=1

# Bundle the Wayland platform plugin in addition to xcb so the app runs natively
# under either session (Qt auto-selects: wayland on Wayland, xcb on X11) — no
# XWayland. linuxdeploy-plugin-qt also deploys the wayland-* integration plugins
# (shell/decoration/graphics) and libQt6WaylandClient when a wayland platform
# plugin is requested. (Arch names the plugin libqwayland.so, not -generic.)
export EXTRA_PLATFORM_PLUGINS="libqwayland.so"

# Tell linuxdeploy NOT to relocate the bundled KF6/Breeze libs out of lib_breeze
# (where breeze6.so resolves them via $ORIGIN), so we keep a single copy of each
# KF6 lib instead of letting linuxdeploy also stamp them into usr/lib. Probed
# against --help so an older linuxdeploy that lacks the flag still works.
EXCLUDES=()
if "$LD" --help 2>&1 | grep -q -- '--exclude-library'; then
    EXCLUDES=(--exclude-library 'libKF6*')
fi

# Curate which Qt imageformats plugins get deployed. The host's kimageformats
# ships a kimg_jxr.so with a dangling libjxrglue.so.0 (no packaged provider),
# which aborts the qt plugin, plus a stack of heavy KDE codecs (avif/exr/aom/…)
# the recorder never uses. We point linuxdeploy-plugin-qt at a curated plugins
# dir: every plugin category is symlinked straight through to the real one,
# except imageformats, which holds only what the app needs (ico for its icon,
# svg for play/pause, plus jpeg/gif). A qmake wrapper rewrites the
# QT_INSTALL_PLUGINS the plugin queries; everything else passes through.
REAL_PLUGINS="$("$QMAKE" -query QT_INSTALL_PLUGINS)"
CURATED_PLUGINS="$TOOLS_DIR/qt-plugins-curated"
rm -rf "$CURATED_PLUGINS"
mkdir -p "$CURATED_PLUGINS/imageformats"
for d in "$REAL_PLUGINS"/*; do
    name="$(basename "$d")"
    [[ "$name" == imageformats ]] && continue
    ln -s "$d" "$CURATED_PLUGINS/$name"
done
for p in libqico.so libqsvg.so libqjpeg.so libqgif.so; do
    [[ -e "$REAL_PLUGINS/imageformats/$p" ]] && \
        ln -s "$REAL_PLUGINS/imageformats/$p" "$CURATED_PLUGINS/imageformats/$p"
done

QMAKE_WRAP="$TOOLS_DIR/qmake-curated"
cat > "$QMAKE_WRAP" <<EOF
#!/usr/bin/env bash
# Rewrites only QT_INSTALL_PLUGINS (both the bare-value and KEY:VALUE -query
# forms); all other qmake queries pass through untouched.
out="\$("$QMAKE" "\$@")"; rc=\$?
printf '%s\n' "\$out" \\
  | sed -e "s|^QT_INSTALL_PLUGINS:.*\$|QT_INSTALL_PLUGINS:$CURATED_PLUGINS|" \\
        -e "s|^$REAL_PLUGINS\$|$CURATED_PLUGINS|"
exit \$rc
EOF
chmod +x "$QMAKE_WRAP"
export QMAKE="$QMAKE_WRAP"

# Deploy Qt into the AppDir (no --output yet — we still need to fix the RPATH
# before packing). No forced platform: with both xcb and wayland plugins bundled,
# Qt picks the right backend for the session on its own.
echo ""
echo "[3/4] Bundling Qt into AppDir..."
"$LD" --appdir "$APPDIR" "${EXCLUDES[@]}" --plugin qt

# linuxdeploy-plugin-qt deploys the wayland shell/decoration plugins but skips
# wayland-graphics-integration-client, which holds the EGL client buffer
# integration Qt needs to hand GPU buffers to the compositor. Without it the app
# runs on Wayland but can't get an OpenGL context, so QCustomPlot falls back to
# slow software rendering. Stage the EGL client plugin (the *-server plugins are
# compositor-side, not needed by a client app) plus the wayland runtime libs it
# pulls in, using the same $ORIGIN rpath layout linuxdeploy gives its plugins.
# libEGL itself stays host-provided (blacklisted) — GPU drivers must come from
# the host, not the bundle.
WGIC_SRC="$REAL_PLUGINS/wayland-graphics-integration-client"
if [[ -e "$WGIC_SRC/libqt-plugin-wayland-egl.so" ]]; then
    WGIC_DST="$APPDIR/usr/plugins/wayland-graphics-integration-client"
    mkdir -p "$WGIC_DST"
    cp "$WGIC_SRC/libqt-plugin-wayland-egl.so" "$WGIC_DST/"
    patchelf --set-rpath '$ORIGIN/../../lib:$ORIGIN' "$WGIC_DST/libqt-plugin-wayland-egl.so"

    # Bundle the wayland runtime libs this plugin (and the wayland platform
    # plugin) need but linuxdeploy left resolving against the host. cp -L
    # dereferences the soname symlink to the real .so.
    for _wllib in libwayland-egl.so.1 libwayland-client.so.0; do
        [[ -e "$APPDIR/usr/lib/$_wllib" ]] && continue
        # NB: no early `exit` in awk — that closes the pipe, SIGPIPEs ldconfig,
        # and with `set -o pipefail`+`set -e` the failed pipeline aborts the script.
        _src="$(ldconfig -p | awk -v n="$_wllib" '$1==n && !seen {print $NF; seen=1}')"
        if [[ -n "$_src" ]]; then
            cp -L "$_src" "$APPDIR/usr/lib/$_wllib"
            patchelf --set-rpath '$ORIGIN' "$APPDIR/usr/lib/$_wllib"
        fi
    done
fi

# The Breeze/KF6 stack pulls in libKF6WindowSystem, which at runtime loads a
# per-display backend plugin from plugins/kf6/kwindowsystem/ (an X11 and a
# KWayland one). The Breeze superbuild ships the lib but not these plugins, and
# the AppImage's qt.conf hides the host's copies, so KWindowSystem prints "Could
# not find any platform plugin". Stage the host plugins — their deps are all
# already bundled (libKF6WindowSystem in lib_breeze; Qt/wayland in usr/lib) — and
# point the rpath at both dirs. KF6 keeps ABI within 6.x, so the host plugins
# load against the bundled lib. From usr/plugins/kf6/kwindowsystem/ that's three
# levels up to usr/, then bin/lib_breeze and lib respectively.
KWS_SRC="$(find /usr/lib /usr/lib64 -maxdepth 4 -type d -path '*kf6/kwindowsystem' 2>/dev/null | head -1)"
if [[ -n "$KWS_SRC" && -d "$APPDIR/usr/bin/lib_breeze" ]]; then
    KWS_DST="$APPDIR/usr/plugins/kf6/kwindowsystem"
    mkdir -p "$KWS_DST"
    cp "$KWS_SRC"/*.so "$KWS_DST/"
    for _kws in "$KWS_DST"/*.so; do
        patchelf --set-rpath '$ORIGIN/../../../bin/lib_breeze:$ORIGIN/../../../lib' "$_kws"
    done
fi

# ── Make the Qt runtime fully self-contained ─────────────────────────────────
# linuxdeploy bundles the Qt libs the app + auto-deployed plugins need, but the
# plugins we stage by hand (wayland-egl, kwindowsystem) and the Breeze/KF6 libs
# in lib_breeze can reference Qt libs that weren't pulled in, leaving them to
# resolve against the host. Walk every ELF in the AppDir and copy any missing
# libQt6*.so out of the host Qt, to a fixed point (a freshly copied Qt lib may
# itself pull in more), each with the $ORIGIN rpath linuxdeploy uses for libs.
QT_LIBS_DIR="$("$QMAKE" -query QT_INSTALL_LIBS)"
while :; do
    _added=0
    while IFS= read -r -d '' _elf; do
        for _need in $(patchelf --print-needed "$_elf" 2>/dev/null || true); do
            case "$_need" in
                libQt6*.so*)
                    if [[ ! -e "$APPDIR/usr/lib/$_need" && -e "$QT_LIBS_DIR/$_need" ]]; then
                        cp -L "$QT_LIBS_DIR/$_need" "$APPDIR/usr/lib/$_need"
                        patchelf --set-rpath '$ORIGIN' "$APPDIR/usr/lib/$_need"
                        _added=1
                    fi
                    ;;
            esac
        done
    done < <(find "$APPDIR/usr" -type f \( -name '*.so' -o -name '*.so.*' \) -print0)
    [[ "$_added" -eq 0 ]] && break
done

# The Breeze style plugin and the KF6 libs in lib_breeze link Qt too, but their
# rpaths only reach within the Breeze bundle, so they'd still load Qt from the
# host. Add the bundled Qt (usr/lib) to their rpaths so the bundle is the sole
# source of Qt. Paths are relative to each file's own location.
if [[ -d "$APPDIR/usr/bin/lib_breeze" ]]; then
    # lib_breeze/foo.so → usr/lib is ../../lib; keep $ORIGIN for KF6 siblings.
    for _kf in "$APPDIR/usr/bin/lib_breeze"/*.so*; do
        patchelf --set-rpath '$ORIGIN:$ORIGIN/../../lib' "$_kf"
    done
    # plugins/styles/breeze6.so → lib_breeze is ../../lib_breeze, usr/lib is ../../../lib.
    _bz="$APPDIR/usr/bin/plugins/styles/breeze6.so"
    [[ -e "$_bz" ]] && patchelf --set-rpath '$ORIGIN/../../lib_breeze:$ORIGIN/../../../lib' "$_bz"
fi

# linuxdeploy rewrites the executable's RPATH while deploying Qt; re-assert
# $ORIGIN/lib_breeze so the app keeps finding the bundled KF6 libs. Must happen
# before packing. Harmless if already present.
if command -v patchelf >/dev/null 2>&1; then
    patchelf --add-rpath '$ORIGIN/lib_breeze' "$APPDIR/usr/bin/track-n-race-recorder" || true
else
    echo "WARNING: patchelf not found — Breeze KF6 libs may not resolve at runtime." >&2
fi

# Pack the finished AppDir.
echo ""
echo "[4/4] Packing AppImage..."
export OUTPUT="Track_N_Race_Background_Recorder-x86_64.AppImage"
"$APPIMAGETOOL" "$APPDIR" "$SCRIPT_DIR/$OUTPUT"

echo ""
echo "============================================="
echo "   AppImage Built (Breeze bundled)!"
echo "============================================="
echo "Output: $SCRIPT_DIR/$OUTPUT"
