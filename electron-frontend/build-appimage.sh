#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_VERSION="${1:?Usage: $0 <version>}"

cd "$SCRIPT_DIR"
npm run build
npm run dist -- --linux AppImage \
    "--config.extraMetadata.version=$APP_VERSION"
