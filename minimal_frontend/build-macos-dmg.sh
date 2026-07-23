#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="${DIST_DIR:-$SCRIPT_DIR/dist/macos}"
DMG_DIR="${DMG_DIR:-$SCRIPT_DIR/dist/dmg}"
DMG_NAME="Track-N-Race - Minimal.dmg"

"$SCRIPT_DIR/build-macos.sh"

mkdir -p "$DMG_DIR"
cmake -E rm -f "$DMG_DIR/$DMG_NAME"
hdiutil create \
    -volname "Track N Race Minimal Recorder" \
    -srcfolder "$DIST_DIR" \
    -ov \
    -format UDZO \
    "$DMG_DIR/$DMG_NAME"

echo "Disk image: $DMG_DIR/$DMG_NAME"
