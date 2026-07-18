#!/usr/bin/env bash

# Build the Electron application and package it as a macOS DMG.
# The standalone native_recorder application is intentionally not built here.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# macOS application bundles and ASAR integrity metadata must be assembled on a
# native macOS filesystem. This project may live on exFAT, which creates
# AppleDouble files and can corrupt Electron Builder's ASAR integrity pass.
BUILD_TEMP_ROOT="${TMPDIR:-/private/tmp}"
BUILD_TEMP_ROOT="${BUILD_TEMP_ROOT%/}"
BUILD_OUTPUT_DIR=""

cleanup() {
    if [[ -n "$BUILD_OUTPUT_DIR" && -d "$BUILD_OUTPUT_DIR" &&
          "$BUILD_OUTPUT_DIR" == "$BUILD_TEMP_ROOT"/track-n-race-macos.* ]]; then
        rm -rf -- "$BUILD_OUTPUT_DIR"
    fi
}
trap cleanup EXIT

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "Error: build-macos.sh must be run on macOS." >&2
    exit 1
fi

if [[ ! -f package.json ]]; then
    echo "Error: package.json not found in $SCRIPT_DIR." >&2
    exit 1
fi

read -r -p "What kind of build is this? (minor/major/overhaul) " BUILD_TYPE
BUILD_TYPE="$(printf '%s' "$BUILD_TYPE" | tr '[:upper:]' '[:lower:]' | xargs)"

CURRENT_VERSION="$(node -p "require('./package.json').version")"
if [[ ! "$CURRENT_VERSION" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
    echo "Error: unexpected version format in package.json ($CURRENT_VERSION). Expected x.y.z." >&2
    exit 1
fi

MAJOR="${BASH_REMATCH[1]}"
MINOR="${BASH_REMATCH[2]}"
PATCH="${BASH_REMATCH[3]}"

case "$BUILD_TYPE" in
    minor)
        PATCH=$((PATCH + 1))
        ;;
    major)
        MINOR=$((MINOR + 1))
        PATCH=0
        ;;
    overhaul)
        MAJOR=$((MAJOR + 1))
        MINOR=0
        PATCH=0
        ;;
    *)
        echo "Error: invalid build type. Expected 'minor', 'major', or 'overhaul'." >&2
        exit 1
        ;;
esac

NEW_VERSION="$MAJOR.$MINOR.$PATCH"
echo "Updating version from $CURRENT_VERSION to $NEW_VERSION..."
npm pkg set "version=$NEW_VERSION"

echo "Building the Electron application..."
echo "> npm run build"
npm run build

echo "Packaging the macOS DMG..."
BUILD_OUTPUT_DIR="$(mktemp -d "$BUILD_TEMP_ROOT/track-n-race-macos.XXXXXX")"
echo "> npm run dist -- --mac dmg --config.directories.output=<temporary macOS directory>"
npm run dist -- --mac dmg "--config.directories.output=$BUILD_OUTPUT_DIR"

ARTIFACT_DIR="$SCRIPT_DIR/dist"
mkdir -p "$ARTIFACT_DIR"
DMG_FOUND=false
while IFS= read -r -d '' ARTIFACT; do
    cp -f "$ARTIFACT" "$ARTIFACT_DIR/"
    if [[ "$ARTIFACT" == *.dmg ]]; then
        DMG_FOUND=true
    fi
done < <(find "$BUILD_OUTPUT_DIR" -maxdepth 1 -type f \
    \( -name '*.dmg' -o -name '*.blockmap' -o -name 'latest-mac.yml' \) -print0)

if [[ "$DMG_FOUND" != true ]]; then
    echo "Error: Electron Builder completed without producing a DMG." >&2
    exit 1
fi

echo "DMG build complete. Artifacts are in: $ARTIFACT_DIR"

read -r -p "Do you want to commit the version upgrade and push it? (y/n) " COMMIT_VERSION
if [[ "$COMMIT_VERSION" =~ ^[Yy] ]]; then
    git add package.json
    git commit -m "chore: bump version to $NEW_VERSION"
    git push
    echo "Successfully committed and pushed package.json."
else
    echo "Skipping commit and push."
fi
