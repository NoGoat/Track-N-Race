#!/usr/bin/env bash
# build.sh — Build the Track N Race Background Recorder on Linux/macOS
# Usage: ./build.sh [--qt-prefix /path/to/Qt/6.x.x/gcc_64]
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QT_PREFIX="${QT_PREFIX:-}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --qt-prefix) QT_PREFIX="$2"; shift 2 ;;
        *) echo "Unknown argument: $1"; exit 1 ;;
    esac
done

echo "============================================="
echo "   TNRD Background Recorder - Build Script"
echo "============================================="

CMAKE_ARGS=(
    -S "$SCRIPT_DIR" -B "$SCRIPT_DIR/build"
    -DCMAKE_BUILD_TYPE=Release
    # Aggressive optimisation: tune for the build host's CPU + interprocedural (LTO).
    -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -march=native -funroll-loops"
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON
)
if [ -n "$QT_PREFIX" ]; then
    CMAKE_ARGS+=(-DCMAKE_PREFIX_PATH="$QT_PREFIX")
fi

echo ""
echo "[1/2] Configuring..."
cmake "${CMAKE_ARGS[@]}"

echo ""
echo "[2/2] Compiling..."
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
cmake --build "$SCRIPT_DIR/build" --config Release -j"$JOBS"

BINARY="$SCRIPT_DIR/build/Track N Race Background Recorder"
if [ -f "$BINARY" ]; then
    echo ""
    echo "============================================="
    echo "   Build Succeeded!"
    echo "============================================="
    echo "Executable: $BINARY"
else
    echo "Build failed: output binary not found."
    exit 1
fi
