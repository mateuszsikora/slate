#!/usr/bin/env bash
#
# Build the editor bundle the firmware serves (design.md §10).
#
# Run from anywhere; paths are resolved relative to this script. Requires node
# and network access on first run.
#
#   tools/editor/build.sh
#
# Output lands in editor/dist/index.html — one document with the JavaScript and
# the CSS inlined, which firmware/components/slate_editor compresses into the
# image and seeds onto LittleFS. It is NOT committed, for the reason
# tools/fonts/generate.sh gives about generated assets: a couple of hundred
# kilobytes of unreviewable output per revision. editor/src is the source of
# truth; run this before building the firmware.
#
# `npm ci` when there is a lock file to install from, because the bundle that
# ships in an image should be the one the lock file describes.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDITOR_DIR="$(cd "${SCRIPT_DIR}/../../editor" && pwd)"

cd "${EDITOR_DIR}"

if [ -f package-lock.json ]; then
    npm ci --no-audit --no-fund
else
    npm install --no-audit --no-fund
fi

npm run build

echo "editor bundle: ${EDITOR_DIR}/dist/index.html"
