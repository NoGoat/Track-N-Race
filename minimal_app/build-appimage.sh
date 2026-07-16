#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR/build-appimage}"
APPDIR="${APPDIR:-$SCRIPT_DIR/AppDir}"
TOOLS_DIR="${TOOLS_DIR:-$SCRIPT_DIR/.appimage-tools}"
OUTPUT_DIR="${OUTPUT_DIR:-$SCRIPT_DIR/dist/appimage}"

LINUXDEPLOY_VERSION="1-alpha-20250213-2"
LINUXDEPLOY="$TOOLS_DIR/linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/${LINUXDEPLOY_VERSION}/linuxdeploy-x86_64.AppImage"

mkdir -p "$TOOLS_DIR" "$OUTPUT_DIR"
if [[ ! -x "$LINUXDEPLOY" ]]; then
    curl --fail --location --retry 3 "$LINUXDEPLOY_URL" --output "$LINUXDEPLOY"
    chmod +x "$LINUXDEPLOY"
fi

cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$BUILD_DIR" --config Release --parallel "$(nproc)"

rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD_DIR" --config Release

export ARCH=x86_64
export APPIMAGE_EXTRACT_AND_RUN=1
export OUTPUT="Track_N_Race_Minimal_Recorder-x86_64.AppImage"
(
    cd "$OUTPUT_DIR"
    "$LINUXDEPLOY" \
        --appdir "$APPDIR" \
        --executable "$APPDIR/usr/bin/track-n-race-minimal" \
        --desktop-file "$APPDIR/usr/share/applications/track-n-race-minimal.desktop" \
        --icon-file "$APPDIR/usr/share/icons/hicolor/256x256/apps/track-n-race-minimal.png" \
        --output appimage
)

echo "AppImage written to: $OUTPUT_DIR/$OUTPUT"
