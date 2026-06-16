#!/usr/bin/env bash
# build.sh — Build the Track N Race Background Recorder on Linux/macOS
# Usage: ./build.sh [--qt-prefix /path/to/Qt/6.x.x/gcc_64]
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QT_PREFIX="${QT_PREFIX:-}"
# Set with --with-breeze to bundle the prebuilt Breeze style stack (see
# breeze_stack/) next to the app. Defaults to the superbuild's standard prefix.
BREEZE_PREFIX=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --qt-prefix) QT_PREFIX="$2"; shift 2 ;;
        # Optional explicit prefix path; bare --with-breeze uses the default.
        --with-breeze)
            if [[ -n "$2" && "$2" != --* ]]; then BREEZE_PREFIX="$2"; shift 2
            else BREEZE_PREFIX="$SCRIPT_DIR/build/breeze_stack/prefix"; shift 1; fi ;;
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
if [ -n "$BREEZE_PREFIX" ]; then
    if [ ! -d "$BREEZE_PREFIX" ]; then
        echo "--with-breeze: prefix not found: $BREEZE_PREFIX"
        echo "Build it first:  cmake -S breeze_stack -B build/breeze_stack -DBREEZE_QT_PREFIX=/usr && cmake --build build/breeze_stack"
        exit 1
    fi
    echo "Bundling Breeze from: $BREEZE_PREFIX"
    CMAKE_ARGS+=(-DBREEZE_STACK_PREFIX="$BREEZE_PREFIX")
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
