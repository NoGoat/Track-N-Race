#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR/build-linux}"
DIST_DIR="${DIST_DIR:-$SCRIPT_DIR/dist/linux}"

cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$DIST_DIR"
cmake --build "$BUILD_DIR" --config Release --parallel "$(nproc)"
cmake --install "$BUILD_DIR" --config Release

echo "System-linked Linux files staged at: $DIST_DIR"
