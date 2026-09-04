#!/usr/bin/env bash
#
# Build the browser flashing page (DESIGN.md §9.1 step 1, §14 M8).
#
#   tools/flasher/build.sh <version> [build-dir] [out-dir]
#
#   version    what the page and the manifest call this image, without a
#              leading `v`: 1.0.0. It is also the directory the images are
#              published under, so two releases cannot share a cached path.
#   build-dir  an ESP-IDF build directory, default firmware/build. It must have
#              been built, because flash_project_args inside it is where the
#              offsets come from — see tools/flasher/manifest.py for why they
#              are not written down here.
#   out-dir    default flasher/dist.
#
# SLATE_CHANNEL_BASE_URL overrides where the result will be published, which is
# what §11.4's update manifest has to name an absolute image URL with. It
# defaults to the project's Pages deployment.
#
# Output is a directory that can be served as-is: the page, the vendored ESP Web
# Tools bundle, both manifests — ESP Web Tools' and §11.4's update channel in
# ota/ — and the images they name. Nothing in it
# refers to another origin, which is the property that matters — a flashing page
# that half-loads is worse than one that does not load at all, and a browser
# refusing a cross-origin fetch of a firmware image is a failure with no visible
# cause on the page where it happens.
#
# Requires node for `npm ci` and network access on its first run, exactly as
# tools/editor/build.sh does. Neither is needed to serve the result.
#
set -euo pipefail

usage() {
    sed -n '3,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
    exit 2
}

[[ $# -ge 1 && $# -le 3 ]] || usage

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
FLASHER_DIR="${REPO_ROOT}/flasher"

VERSION="$1"
# Resolved against the caller's directory before anything cd's, so a relative
# `firmware/build-release` means what it looked like it meant when it was typed.
absolute() { case "$1" in /*) printf '%s\n' "$1" ;; *) printf '%s\n' "${PWD}/$1" ;; esac; }
BUILD_DIR="$(absolute "${2:-${REPO_ROOT}/firmware/build}")"
OUT_DIR="$(absolute "${3:-${FLASHER_DIR}/dist}")"

# Where this directory is going to be served from, which is the one thing a
# build cannot know and §11.4's manifest cannot do without: it is the URL the
# panel downloads the image from. The default is the project's own Pages
# deployment and the same address the firmware checks by default
# (firmware/components/slate_update/slate_update.c); the release workflow
# derives it from the repository instead of trusting this copy, and a local
# HTTPS test server is the other reason it is a variable.
CHANNEL_BASE_URL="${SLATE_CHANNEL_BASE_URL:-https://mateuszsikora.github.io/slate/}"

# `v1.0.0` is the tag; `1.0.0` is the version. §11.4's manifest and §4.1's
# firmware_version both carry the second form, and a page that showed the first
# would be the only place in the project that spells it differently.
if [[ "${VERSION}" == v* ]]; then
    echo "version should not carry the tag's leading v: ${VERSION#v}, not ${VERSION}" >&2
    exit 1
fi

cd "${FLASHER_DIR}"
if [ -f package-lock.json ]; then
    npm ci --no-audit --no-fund
else
    npm install --no-audit --no-fund
fi

rm -rf "${OUT_DIR}"
mkdir -p "${OUT_DIR}"
cp "${FLASHER_DIR}/index.html" "${OUT_DIR}/index.html"

# The whole of dist/web, not install-button.js alone: the bundle loads the
# per-chip flasher stubs as separate modules at the moment they are needed, so
# copying the entry point on its own produces a page that works until somebody
# presses the button.
cp -R "${FLASHER_DIR}/node_modules/esp-web-tools/dist/web" "${OUT_DIR}/esp-web-tools"
cp "${FLASHER_DIR}/node_modules/esp-web-tools/LICENSE" "${OUT_DIR}/esp-web-tools/LICENSE"

echo "images from ${BUILD_DIR}:"
python3 "${SCRIPT_DIR}/manifest.py" \
    --build-dir "${BUILD_DIR}" \
    --version "${VERSION}" \
    --out "${OUT_DIR}" \
    --repo "${REPO_ROOT}"

# §11.4's manifest, into the same directory and from the same images. The panel
# and the browser install one release from one deployment; see
# tools/ota/manifest.py for why this is not a job of its own.
python3 "${REPO_ROOT}/tools/ota/manifest.py" \
    --dist "${OUT_DIR}" \
    --version "${VERSION}" \
    --base-url "${CHANNEL_BASE_URL}" \
    --repo "${REPO_ROOT}"

echo "flasher: ${OUT_DIR}"
echo "serve it with: python3 -m http.server --directory ${OUT_DIR}"
