#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 <version-name>" >&2
    exit 2
fi

VERSION_NAME="$1"
if [[ ! "$VERSION_NAME" =~ ^[0-9A-Za-z][0-9A-Za-z._-]*$ ]]; then
    echo "Error: version-name may contain only letters, numbers, dots, underscores, and hyphens." >&2
    exit 2
fi

RELEASE_DIR="$ROOT_DIR/release-$VERSION_NAME"
if [[ -e "$RELEASE_DIR" ]]; then
    echo "Error: release directory already exists: $RELEASE_DIR" >&2
    exit 1
fi

echo "Building macOS release '$VERSION_NAME'..."

echo
echo "[1/2] Building Electron DMG..."
(
    cd "$ROOT_DIR/electron-frontend"
    export INCLUDE_PERFORMANCE_DIAGNOSTICS=0
    npm run build
    npm run dist -- --mac dmg
)

echo
echo "[2/2] Building minimal DMG..."
bash "$ROOT_DIR/minimal_frontend/build-macos-dmg.sh"

ARTIFACTS=(
    "$ROOT_DIR/electron-frontend/dist/Track-N-Race - Electron.dmg"
    "$ROOT_DIR/minimal_frontend/dist/dmg/Track-N-Race - Minimal.dmg"
)

for artifact in "${ARTIFACTS[@]}"; do
    if [[ ! -f "$artifact" ]]; then
        echo "Error: expected build artifact was not produced: $artifact" >&2
        exit 1
    fi
done

mkdir "$RELEASE_DIR"
for artifact in "${ARTIFACTS[@]}"; do
    cp "$artifact" "$RELEASE_DIR/"
done

echo
echo "macOS release complete: $RELEASE_DIR"
