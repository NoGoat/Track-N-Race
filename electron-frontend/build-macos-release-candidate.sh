#!/usr/bin/env bash

# Build an Electron-only macOS release candidate without changing package.json.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

cleanup() {
    unset RC_VERSION 2>/dev/null || true
    unset INCLUDE_PERFORMANCE_DIAGNOSTICS 2>/dev/null || true
}
trap cleanup EXIT

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "Error: build-macos-release-candidate.sh must be run on macOS." >&2
    exit 1
fi

if [[ ! -f package.json ]]; then
    echo "Error: package.json not found in $SCRIPT_DIR." >&2
    exit 1
fi

read -r -p "What version should the version panel show? (e.g. 1.3.0-rc1) " RC_VERSION
if [[ -z "${RC_VERSION//[[:space:]]/}" ]]; then
    echo "Error: version cannot be empty." >&2
    exit 1
fi

export RC_VERSION

read -r -p "Include performance diagnostics (React Scan + stats-gl)? (y/N) " DIAGNOSTICS_CHOICE
if [[ "$DIAGNOSTICS_CHOICE" =~ ^[Yy]$ ]]; then
    export INCLUDE_PERFORMANCE_DIAGNOSTICS=1
    DIAGNOSTICS_STATUS="enabled"
else
    export INCLUDE_PERFORMANCE_DIAGNOSTICS=0
    DIAGNOSTICS_STATUS="disabled"
fi

echo "Building macOS release candidate '$RC_VERSION' with performance diagnostics $DIAGNOSTICS_STATUS (package.json is left untouched)..."

echo "Building the Electron application..."
echo "> npm run build"
npm run build

ARTIFACT_DIR="$SCRIPT_DIR/dist"
mkdir -p "$ARTIFACT_DIR"

# extraMetadata changes the packaged app version and artifact filename without
# writing the release-candidate version back to package.json.
echo "> npm run dist -- --mac dmg --config.extraMetadata.version=$RC_VERSION --config.directories.output=$ARTIFACT_DIR"
npm run dist -- --mac dmg \
    "--config.extraMetadata.version=$RC_VERSION" \
    "--config.directories.output=$ARTIFACT_DIR"

DMG_FOUND=false
while IFS= read -r -d '' ARTIFACT; do
    if [[ "$ARTIFACT" == *.dmg ]]; then
        DMG_FOUND=true
    fi
done < <(find "$ARTIFACT_DIR" -maxdepth 1 -type f \
    \( -name '*.dmg' -o -name '*.blockmap' -o -name 'latest-mac.yml' \) -print0)

if [[ "$DMG_FOUND" != true ]]; then
    echo "Error: Electron Builder completed without producing a DMG." >&2
    exit 1
fi

echo "Release candidate '$RC_VERSION' DMG complete."
echo "Artifacts are in: $ARTIFACT_DIR"
