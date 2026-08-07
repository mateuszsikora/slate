#!/usr/bin/env bash
#
# Install a firmware image on a Slate panel over the network (design.md §11.1).
#
#   SLATE_TOKEN=... tools/ota/upload.sh <host> [image]
#
#   host    an address, a name, or a full URL: 192.168.1.42, slate-a1b2c3.local,
#           http://192.168.1.42. The setup access point at 192.168.4.1 answers
#           the same endpoint (§11.2), so a panel with no router is not stranded.
#   image   defaults to firmware/build/slate.bin, which is what `idf.py build`
#           produces for the project name fixed in firmware/CMakeLists.txt.
#
# The device token comes from $SLATE_TOKEN and is handed to curl over a config
# file on stdin rather than as an argument, so it does not appear in the process
# list of a shared machine. It is never printed. §4.3 delivers it through the
# pairing QR on the panel; until #36 renders that, it is read out of NVS with
# `idf.py monitor` on a device you already have a cable to.
#
# This is a development tool and nothing else: no manifest, no signature, no
# HTTPS. §11.4's release OTA is a separate mechanism for a separate audience.
#
# Exit status is 0 only when the panel came back answering with the version that
# was uploaded — a curl that returned 200 proves the image was accepted, not
# that it boots, and this script is the thing standing between a desk and a
# board on a wall.

set -euo pipefail

usage() {
    sed -n '3,11p' "$0" | cut -c 3-
    exit 2
}

[ $# -ge 1 ] && [ $# -le 2 ] || usage

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HOST="$1"
IMAGE="${2:-$REPO_ROOT/firmware/build/slate.bin}"
: "${SLATE_TOKEN:?set SLATE_TOKEN to the device token (design.md §4.3)}"

case "$HOST" in
    http://*|https://*) BASE="$HOST" ;;
    *)                  BASE="http://$HOST" ;;
esac
API="$BASE/api/v1"

# Enough to distinguish "the panel is not there" from "the panel refused the
# image", which are the two failures worth telling apart at a glance.
if [ ! -f "$IMAGE" ]; then
    echo "no image at $IMAGE — run \`idf.py build\` in firmware/ first" >&2
    exit 1
fi

# The same first-byte check the firmware does before it erases anything. Doing
# it here too costs nothing and saves uploading a megabyte of the wrong file
# over WiFi to be told so.
if [ "$(head -c 1 "$IMAGE" | od -An -tx1 | tr -d ' ')" != "e9" ]; then
    echo "$IMAGE is not an ESP application image (no 0xE9 magic)" >&2
    exit 1
fi

# One field out of a small, flat JSON document. A jq dependency for this would
# be the only one this repository has.
json_field() {
    sed -n 's/.*"'"$1"'"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p'
}

authenticated_curl() {
    printf 'header = "Authorization: Bearer %s"\n' "$SLATE_TOKEN" |
        curl --config - "$@"
}

before="$(curl -fsS --connect-timeout 5 --max-time 10 "$API/info" | json_field firmware_version || true)"
echo "panel at $BASE is running ${before:-an unknown version}"
echo "uploading $(basename "$IMAGE") ($(wc -c < "$IMAGE" | tr -d ' ') bytes)"

body="$(mktemp)"
trap 'rm -f "$body"' EXIT

# `Expect:` is emptied deliberately. curl adds `Expect: 100-continue` to a body
# this size, esp_http_server never answers it, and curl then waits out its
# one-second timeout before sending anything — a second of apparent hang on
# every flash, for a handshake that buys nothing here.
status="$(authenticated_curl \
    --request POST \
    --data-binary "@$IMAGE" \
    --header 'Content-Type: application/octet-stream' \
    --header 'Expect:' \
    --connect-timeout 5 \
    --max-time 300 \
    --progress-bar \
    --output "$body" \
    --write-out '%{http_code}' \
    "$API/ota/upload")"

if [ "$status" != "200" ]; then
    echo "upload refused: HTTP $status $(json_field error < "$body")" >&2
    exit 1
fi

uploaded="$(json_field version < "$body")"
echo "accepted into $(json_field partition < "$body"): $uploaded — waiting for the reboot"

# The panel drops the connection, reboots, re-associates and brings the HTTP
# server back up. A DHCP lease it already holds usually comes back within a few
# seconds; 60 s is the same budget §11.2 gives the health check.
for _ in $(seq 60); do
    sleep 1
    running="$(curl -fsS --connect-timeout 2 --max-time 5 "$API/info" 2>/dev/null |
        json_field firmware_version || true)"
    [ -n "$running" ] || continue

    if [ "$running" = "$uploaded" ]; then
        echo "panel is running $running"
        exit 0
    fi
    # Answering with the old version means the image was written but something
    # sent the panel back to it — which is what #12's rollback will look like
    # from here once it exists.
    echo "panel came back running $running, not $uploaded" >&2
    exit 1
done

echo "panel did not answer within 60 s of the upload" >&2
exit 1
