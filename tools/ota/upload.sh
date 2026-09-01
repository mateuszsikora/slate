#!/usr/bin/env bash
#
# Install a firmware image on a Slate panel over the network (design.md §11.1).
#
#   SLATE_TOKEN=... tools/ota/upload.sh <host> [image]
#
# The device token comes from $SLATE_TOKEN and is handed to curl over a config
# file on stdin rather than as an argument, so it does not appear in the process
# list of a shared machine. It is never printed. The editor obtains it through
# §4.3's session bootstrap and keeps it in memory; development automation may
# read it from a trusted local secret store. A legacy panel can be recovered
# without changing its configuration by reading the NVS partition over USB:
#
#   esptool.py -p <port> read_flash 0x9000 0x14000 nvs.bin
#   python $IDF_PATH/components/nvs_flash/nvs_partition_tool/nvs_tool.py nvs.bin
#
# and `dev_token` is in the dump. Delete the dump afterwards.
#
# This is a development tool and nothing else: no manifest, no signature, no
# HTTPS. §11.4's release OTA is a separate mechanism for a separate audience.
#
# What exit 0 proves, and what it does not. The script waits for the panel to
# answer again and compares the version it reports against the version that was
# uploaded, because a 200 proves the image was accepted, not that it boots. That
# comparison is only as sharp as the version string: ESP-IDF derives it from
# `git describe --always --tags --dirty`, so two builds of the same working tree
# carry the same string and the check degrades to "the panel came back". The
# script says so when it happens rather than implying a proof it cannot give.

set -euo pipefail

usage() {
    cat >&2 <<'EOF'
Install a firmware image on a Slate panel over the network (design.md §11.1).

  SLATE_TOKEN=... tools/ota/upload.sh <host> [image]

  host    an address, a name, or a full URL: 192.168.1.42, slate-a1b2c3.local,
          http://192.168.1.42. The setup access point at 192.168.4.1 answers
          the same endpoint (§11.2), so a panel with no router is not stranded.
  image   defaults to firmware/build/slate.bin, which is what `idf.py build`
          produces for the project name fixed in firmware/CMakeLists.txt.
EOF
    exit 2
}

[[ $# -ge 1 && $# -le 2 ]] || usage

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
HOST="$1"
IMAGE="${2:-${REPO_ROOT}/firmware/build/slate.bin}"
: "${SLATE_TOKEN:?set SLATE_TOKEN to the device token (design.md §4.3)}"

case "${HOST}" in
    http://*|https://*) BASE="${HOST}" ;;
    *)                  BASE="http://${HOST}" ;;
esac
API="${BASE}/api/v1"

# How long to wait for the panel after the reboot. Wall clock, not a count of
# attempts: a panel that is not answering costs a connect timeout per attempt,
# so counting iterations would report "60 s" after three minutes. Same budget
# §11.2 gives the health check.
REBOOT_WAIT_S=60

if [[ ! -f "${IMAGE}" ]]; then
    echo "no image at ${IMAGE} — run \`idf.py build\` in firmware/ first" >&2
    exit 1
fi

# The same header the firmware checks before it erases anything: 0xE9 at byte 0
# and the chip id at byte 12 (0x09 is ESP32-S3). Both, not just the magic —
# an ESP32 image also starts with 0xE9, and that is the mistake worth catching
# before a megabyte goes over WiFi rather than after.
header="$(od -An -tx1 -N 14 "${IMAGE}" | tr -d ' \n')"
if [[ "${header:0:2}" != "e9" ]]; then
    echo "${IMAGE} is not an ESP application image (no 0xE9 magic)" >&2
    exit 1
fi
if [[ "${header:24:4}" != "0900" ]]; then
    echo "${IMAGE} is not an ESP32-S3 image (chip id ${header:24:4}) — wrong board?" >&2
    exit 1
fi

# python3 rather than a sed expression: it is already a hard dependency of
# idf.py and of the other two scripts in tools/, and a regex that is blind to
# the shape of what it reads reports "the panel is not answering" for a
# document it merely did not recognise.
json_field() {
    python3 -c 'import json,sys
try:
    print(json.load(sys.stdin).get(sys.argv[1], ""))
except Exception:
    pass' "$1"
}

authenticated_curl() {
    printf 'header = "Authorization: Bearer %s"\n' "${SLATE_TOKEN}" |
        curl --config - "$@"
}

before="$(curl -fsS --connect-timeout 5 --max-time 10 "${API}/info" 2>/dev/null |
    json_field firmware_version || true)"
echo "panel at ${BASE} is running ${before:-an unknown version}"
echo "uploading $(basename "${IMAGE}") ($(wc -c < "${IMAGE}" | tr -d ' ') bytes)"

body="$(mktemp)"
trap 'rm -f "${body}"' EXIT

# `Expect:` is emptied deliberately. curl adds `Expect: 100-continue` to a body
# this size, esp_http_server never answers it, and curl then waits out its
# one-second timeout before sending anything — a second of apparent hang on
# every flash, for a handshake that buys nothing here.
#
# The status is captured with `set -e` suspended: a transport failure (no route,
# name does not resolve, connection reset mid-upload) is curl's exit code, and
# letting it abort the script here would skip the diagnostic below — which is
# the one distinction this script exists to make.
set +e
status="$(authenticated_curl \
    --request POST \
    --data-binary "@${IMAGE}" \
    --header 'Content-Type: application/octet-stream' \
    --header 'Expect:' \
    --connect-timeout 5 \
    --max-time 300 \
    --progress-bar \
    --output "${body}" \
    --write-out '%{http_code}' \
    "${API}/ota/upload")"
curl_status=$?
set -e

if [[ ${curl_status} -ne 0 ]]; then
    echo "could not upload to ${BASE}: curl exit ${curl_status} — the panel did not take the" \
         "image, and nothing on it changed unless the connection died mid-transfer" >&2
    exit 1
fi
if [[ "${status}" != "200" ]]; then
    echo "upload refused: HTTP ${status} $(json_field error < "${body}")" >&2
    exit 1
fi

uploaded="$(json_field version < "${body}")"
partition="$(json_field partition < "${body}")"
echo "accepted${partition:+ into ${partition}}${uploaded:+: ${uploaded}} — waiting for the reboot"

# The 200 is the outcome; the body is detail the device drops when it cannot
# allocate it. Say which question the wait can still answer rather than
# comparing against an empty string and calling the result a rollback.
if [[ -z "${uploaded}" ]]; then
    echo "note: the panel accepted the image but did not report its version," \
         "so what follows confirms it came back, not which image booted." >&2
elif [[ -n "${before}" && "${before}" == "${uploaded}" ]]; then
    echo "note: the uploaded image reports the same version as the one it replaced," \
         "so what follows confirms the panel came back, not which image booted." \
         "Commit, or set PROJECT_VER, to tell two builds apart." >&2
fi

# The panel drops the connection, reboots, re-associates and brings the HTTP
# server back up. A DHCP lease it already holds usually comes back within a few
# seconds.
deadline=$((SECONDS + REBOOT_WAIT_S))
while [[ ${SECONDS} -lt ${deadline} ]]; do
    sleep 1
    running="$(curl -fsS --connect-timeout 2 --max-time 5 "${API}/info" 2>/dev/null |
        json_field firmware_version || true)"
    [[ -n "${running}" ]] || continue

    if [[ -z "${uploaded}" || "${running}" == "${uploaded}" ]]; then
        echo "panel is running ${running}"
        exit 0
    fi
    # Answering with the old version means the image was written but something
    # sent the panel back to it — which is what #12's rollback will look like
    # from here once it exists.
    echo "panel came back running ${running}, not ${uploaded}" >&2
    exit 1
done

echo "panel did not answer within ${REBOOT_WAIT_S} s of the upload" >&2
exit 1
