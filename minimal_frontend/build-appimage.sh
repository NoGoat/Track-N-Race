#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ $# -ne 2 || "$1" != "--version" ]]; then
    echo "Usage: $0 --version <major.minor.patch>" >&2
    exit 2
fi
APP_VERSION="$2"
if [[ ! "$APP_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Error: version must use major.minor.patch format (for example, 1.2.0)." >&2
    exit 2
fi

BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR/build-appimage}"
APPDIR="${APPDIR:-$SCRIPT_DIR/AppDir}"
TOOLS_DIR="${TOOLS_DIR:-$SCRIPT_DIR/.appimage-tools}"
OUTPUT_DIR="${OUTPUT_DIR:-$SCRIPT_DIR/dist/appimage}"

LINUXDEPLOY_VERSION="1-alpha-20250213-2"
LINUXDEPLOY="$TOOLS_DIR/linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/${LINUXDEPLOY_VERSION}/linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_QT="$TOOLS_DIR/linuxdeploy-plugin-qt-x86_64.AppImage"
LINUXDEPLOY_QT_URL="https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage"
APPIMAGETOOL="$TOOLS_DIR/appimagetool-x86_64.AppImage"
APPIMAGETOOL_URL="https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage"
APPIMAGETOOL_ROOT="$TOOLS_DIR/appimagetool-root"
APPIMAGE_RUNTIME="$TOOLS_DIR/runtime-x86_64"

mkdir -p "$TOOLS_DIR" "$OUTPUT_DIR"
if [[ ! -x "$LINUXDEPLOY" ]]; then
    curl --fail --location --retry 3 "$LINUXDEPLOY_URL" --output "$LINUXDEPLOY"
    chmod +x "$LINUXDEPLOY"
fi
if [[ ! -x "$LINUXDEPLOY_QT" ]]; then
    curl --fail --location --retry 3 "$LINUXDEPLOY_QT_URL" --output "$LINUXDEPLOY_QT"
    chmod +x "$LINUXDEPLOY_QT"
fi
if [[ ! -x "$APPIMAGETOOL" ]]; then
    curl --fail --location --retry 3 "$APPIMAGETOOL_URL" --output "$APPIMAGETOOL"
    chmod +x "$APPIMAGETOOL"
fi

export QMAKE="${QMAKE:-$(command -v qmake6 || command -v qmake || command -v qmake-qt5 || true)}"
if [[ -z "$QMAKE" ]]; then
    echo "qmake was not found; install the Qt 6 or Qt 5 development tools" >&2
    exit 1
fi

QT_VERSION="$($QMAKE -query QT_VERSION)"
REAL_PLUGINS="$($QMAKE -query QT_INSTALL_PLUGINS)"
CURATED_PLUGINS="$TOOLS_DIR/qt-plugins-curated"
rm -rf "$CURATED_PLUGINS"
mkdir -p "$CURATED_PLUGINS/imageformats"
# Preserve the desktop/platform integration categories, but avoid deploying
# every image codec installed on the build host. Some KDE codecs have optional
# runtime dependencies and can abort linuxdeploy even though this app never
# uses them.
for directory in "$REAL_PLUGINS"/*; do
    name="$(basename "$directory")"
    [[ "$name" == imageformats ]] && continue
    ln -s "$directory" "$CURATED_PLUGINS/$name"
done
for plugin in libqico.so libqsvg.so libqjpeg.so libqgif.so; do
    [[ -e "$REAL_PLUGINS/imageformats/$plugin" ]] && \
        ln -s "$REAL_PLUGINS/imageformats/$plugin" "$CURATED_PLUGINS/imageformats/$plugin"
done

REAL_QMAKE="$QMAKE"
QMAKE_WRAPPER="$TOOLS_DIR/qmake-curated"
cat > "$QMAKE_WRAPPER" <<EOF
#!/usr/bin/env bash
out="\$("$REAL_QMAKE" "\$@")"; rc=\$?
printf '%s\n' "\$out" \\
  | sed -e "s|^QT_INSTALL_PLUGINS:.*\$|QT_INSTALL_PLUGINS:$CURATED_PLUGINS|" \\
        -e "s|^$REAL_PLUGINS\$|$CURATED_PLUGINS|"
exit \$rc
EOF
chmod +x "$QMAKE_WRAPPER"
export QMAKE="$QMAKE_WRAPPER"

cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DTNR_APP_VERSION="$APP_VERSION"
cmake --build "$BUILD_DIR" --config Release --parallel "$(nproc)"

rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD_DIR" --config Release

export ARCH=x86_64
export APPIMAGE_EXTRACT_AND_RUN=1
export NO_STRIP=1
# Bundle native Wayland alongside XCB. The Qt deployment plugin also pulls in
# the Wayland shell/decoration integrations required by that platform plugin.
if [[ "$QT_VERSION" == 5.* ]]; then
    export EXTRA_PLATFORM_PLUGINS="libqwayland-generic.so"
else
    export EXTRA_PLATFORM_PLUGINS="libqwayland.so"
fi
export OUTPUT="Track-N-Race - Minimal.AppImage"
"$LINUXDEPLOY" \
    --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/track-n-race-minimal" \
    --desktop-file "$APPDIR/usr/share/applications/track-n-race-minimal.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/256x256/apps/track-n-race-minimal.png" \
    --plugin qt

# The current appimagetool AppRun wrapper crashes on some newer
# kernels/filesystems. Extract the AppImage and invoke its static binary
# directly, supplying the embedded runtime explicitly.
rm -rf "$APPIMAGETOOL_ROOT"
mkdir -p "$APPIMAGETOOL_ROOT"
(
    cd "$APPIMAGETOOL_ROOT"
    "$APPIMAGETOOL" --appimage-extract >/dev/null
)
APPIMAGE_OFFSET="$("$APPIMAGETOOL" --appimage-offset)"
dd if="$APPIMAGETOOL" of="$APPIMAGE_RUNTIME" bs=1 count="$APPIMAGE_OFFSET" status=none
APPIMAGETOOL_BIN="$APPIMAGETOOL_ROOT/squashfs-root/usr/bin/appimagetool"
APPIMAGETOOL_BIN_DIR="$APPIMAGETOOL_ROOT/squashfs-root/usr/bin"
PATH="$APPIMAGETOOL_BIN_DIR:$PATH" \
    "$APPIMAGETOOL_BIN" --runtime-file "$APPIMAGE_RUNTIME" \
    "$APPDIR" "$OUTPUT_DIR/$OUTPUT"

echo "AppImage written to: $OUTPUT_DIR/$OUTPUT"
