#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR/build-minimal-linux}"
OUTPUT_DIR="${OUTPUT_DIR:-$SCRIPT_DIR/dist/minimal-linux}"
JOBS="${JOBS:-$(nproc)}"

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "This script must be run on Linux." >&2
    exit 1
fi

cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build "$BUILD_DIR" \
    --config MinSizeRel \
    --target minimal_app \
    --parallel "$JOBS"

mkdir -p "$OUTPUT_DIR"
cmake -E copy_if_different \
    "$BUILD_DIR/track-n-race-minimal" \
    "$OUTPUT_DIR/track-n-race-minimal"
cmake -E copy_if_different \
    "$SCRIPT_DIR/README-minimal-linux.md" \
    "$OUTPUT_DIR/README.md"

echo "Executable: $OUTPUT_DIR/track-n-race-minimal"
echo "Dependencies: $OUTPUT_DIR/README.md"
