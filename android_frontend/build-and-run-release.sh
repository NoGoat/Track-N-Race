#!/usr/bin/env bash
# build-and-run-release.sh — build-and-run.sh with the Release build type,
# signed with the release key as build-signed-apk.sh does.
# Accepts the same --device-serial, --sdk-root and --skip-build options.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$SCRIPT_DIR/build-and-run.sh" "$@" --build-type Release --release-key
