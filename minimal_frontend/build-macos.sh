#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR/build-macos}"
DIST_DIR="${DIST_DIR:-$SCRIPT_DIR/dist/macos}"
CONFIG="${CONFIG:-Release}"
ARCHITECTURE="${ARCHITECTURE:-$(uname -m)}"
DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET:-13.0}"
CODESIGN_IDENTITY="${CODESIGN_IDENTITY:--}"
APP_NAME="Track N Race Minimal Recorder.app"

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "This script must be run on macOS." >&2
    exit 1
fi
if [[ "$ARCHITECTURE" != "arm64" && "$ARCHITECTURE" != "x86_64" ]]; then
    echo "ARCHITECTURE must be arm64 or x86_64." >&2
    exit 1
fi

dist_root="$SCRIPT_DIR/dist"
case "$DIST_DIR" in
    "$dist_root"/*) ;;
    *)
        echo "DIST_DIR must remain under $dist_root." >&2
        exit 1
        ;;
esac

cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$CONFIG" \
    -DCMAKE_OSX_ARCHITECTURES="$ARCHITECTURE" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
    -DCMAKE_INSTALL_PREFIX="$DIST_DIR"
cmake --build "$BUILD_DIR" --config "$CONFIG" --target minimal_app --parallel

cmake -E rm -rf "$DIST_DIR"
cmake --install "$BUILD_DIR" --config "$CONFIG" --component MinimalApp

app_path="$DIST_DIR/$APP_NAME"
if [[ "$CODESIGN_IDENTITY" == "-" ]]; then
    codesign --force --deep --sign - "$app_path"
else
    codesign --force --deep --options runtime --timestamp \
        --sign "$CODESIGN_IDENTITY" "$app_path"
fi
codesign --verify --deep --strict "$app_path"

echo "Application: $app_path"
echo "Architecture: $ARCHITECTURE"
