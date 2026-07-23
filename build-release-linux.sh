#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 <version-number>" >&2
    exit 2
fi

VERSION_NUMBER="$1"
if [[ ! "$VERSION_NUMBER" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Error: version-number must use major.minor.patch format (for example, 1.2.0)." >&2
    exit 2
fi

RELEASE_ROOT="$ROOT_DIR/release"
RELEASE_DIR="$RELEASE_ROOT/$VERSION_NUMBER"
if [[ -e "$RELEASE_DIR" ]]; then
    echo "Error: release directory already exists: $RELEASE_DIR" >&2
    exit 1
fi

export TNR_APP_VERSION="$VERSION_NUMBER"
export RC_VERSION="$VERSION_NUMBER"
export INCLUDE_PERFORMANCE_DIAGNOSTICS=0

echo "Building Linux release '$VERSION_NUMBER'..."

echo
echo "[1/3] Building Electron AppImage..."
(
    cd "$ROOT_DIR/electron-frontend"
    npm run build
    npm run dist -- --linux AppImage \
        "--config.extraMetadata.version=$VERSION_NUMBER"
)

echo
echo "[2/3] Building Qt AppImage..."
bash "$ROOT_DIR/qt_frontend/build-appimage.sh"

echo
echo "[3/3] Building minimal AppImage..."
bash "$ROOT_DIR/minimal_frontend/build-appimage.sh"

ARTIFACTS=(
    "$ROOT_DIR/electron-frontend/dist/Track-N-Race - Electron.AppImage"
    "$ROOT_DIR/qt_frontend/Track-N-Race - Qt.AppImage"
    "$ROOT_DIR/minimal_frontend/dist/appimage/Track-N-Race - Minimal.AppImage"
)

for artifact in "${ARTIFACTS[@]}"; do
    if [[ ! -f "$artifact" ]]; then
        echo "Error: expected build artifact was not produced: $artifact" >&2
        exit 1
    fi
done

mkdir -p "$RELEASE_ROOT"
mkdir "$RELEASE_DIR"
for artifact in "${ARTIFACTS[@]}"; do
    cp "$artifact" "$RELEASE_DIR/"
done

echo
echo "Linux release complete: $RELEASE_DIR"
