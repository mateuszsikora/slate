#!/usr/bin/env bash
#
# Fetch a core dump off a Slate panel and symbolicate it (DESIGN.md §11.3).
#
#   SLATE_TOKEN=... tools/coredump/fetch.sh <host> [elf] [output]
#
# The device token is handled exactly as tools/ota/upload.sh handles it: taken
# from $SLATE_TOKEN and handed to curl over a config file on stdin rather than as
# an argument, so it does not appear in the process list of a shared machine, and
# never printed. That script's header describes the browser-session and
# read-only USB recovery paths.
#
# What comes back is the flash image the panic handler wrote: ESP-IDF's header,
# an ELF core file, and a checksum. `esp-coredump --core-format raw` is what
# reads that, and it is the same artifact `idf.py coredump-info` pulls over a
# cable, so a dump fetched over WiFi and a dump read over USB are one format.
#
# THE ELF HAS TO MATCH. gdb symbolicates the dump against the .elf of the image
# that crashed, and §11.2 rolls a panicking image back — so the panel answering
# this request is routinely NOT running the firmware that produced the dump. The
# panel's boot log says which image it was ("image <sha> (NOT this firmware)"),
# and that is the string to check against the .elf being passed here. A mismatch
# does not fail: gdb walks the wrong functions and prints a backtrace that looks
# entirely plausible.
#
# This is a development tool, like the OTA script beside it. It does not erase
# the dump afterwards: the panel keeps it until the next panic overwrites it,
# which is what makes a fetch over bad WiFi repeatable.

set -euo pipefail

usage() {
    cat >&2 <<'EOF'
Fetch a core dump off a Slate panel and symbolicate it (DESIGN.md §11.3).

  SLATE_TOKEN=... tools/coredump/fetch.sh <host> [elf] [output]

  host    an address, a name, or a full URL: 192.168.1.42, slate-a1b2c3.local,
          http://192.168.1.42. The setup access point at 192.168.4.1 answers the
          same endpoint, so a panel with no router is not stranded.
  elf     the .elf of the image that CRASHED, for symbolication. Defaults to
          firmware/build/slate.elf. Pass "-" to download the dump and stop.
  output  where to write the dump. Defaults to firmware/build/coredump.bin.
EOF
    exit 2
}

[[ $# -ge 1 && $# -le 3 ]] || usage

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
HOST="$1"
ELF="${2:-${REPO_ROOT}/firmware/build/slate.elf}"
OUTPUT="${3:-${REPO_ROOT}/firmware/build/coredump.bin}"
: "${SLATE_TOKEN:?set SLATE_TOKEN to the device token (DESIGN.md §4.3)}"

case "${HOST}" in
    http://*|https://*) BASE="${HOST}" ;;
    *)                  BASE="http://${HOST}" ;;
esac
API="${BASE}/api/v1"

authenticated_curl() {
    printf 'header = "Authorization: Bearer %s"\n' "${SLATE_TOKEN}" |
        curl --config - "$@"
}

# Same shape and the same reasoning as tools/ota/upload.sh's helper: python3 is
# already a hard dependency of idf.py and of every script in tools/, and a regex
# blind to the shape of what it reads reports "the panel is not answering" for a
# document it merely did not recognise.
json_field() {
    python3 -c 'import json,sys
try:
    print(json.load(sys.stdin).get(sys.argv[1], ""))
except Exception:
    pass' "$1"
}

# Downloaded beside the destination and moved onto it only after the 200, which is
# upload.sh's arrangement and here it protects something: `curl --output` truncates
# its target the moment the connection opens, so writing straight to ${OUTPUT}
# would let a 404 — the ordinary answer for a panel that has not crashed — destroy
# a dump that was fetched to that path earlier. The panel is the only other copy,
# and only until the next panic overwrites it. In the same directory so the move
# is a rename rather than a copy across filesystems.
mkdir -p "$(dirname "${OUTPUT}")"
body="$(mktemp "${OUTPUT}.XXXXXX")"
trap 'rm -f "${body}"' EXIT

# The status and the body are both needed: a 404 carries §4's
# {"error":"no_coredump"}, which is the answer to "has this panel ever crashed"
# and not a failure of this script.
set +e
status="$(authenticated_curl \
    --connect-timeout 5 \
    --max-time 120 \
    --output "${body}" \
    --write-out '%{http_code}' \
    "${API}/coredump")"
curl_status=$?
set -e

if [[ ${curl_status} -ne 0 ]]; then
    echo "could not fetch from ${BASE}: curl exit ${curl_status}" >&2
    exit 1
fi
if [[ "${status}" == "404" ]]; then
    echo "no core dump on ${BASE} — nothing has panicked since the partition was last erased" >&2
    exit 1
fi
if [[ "${status}" != "200" ]]; then
    echo "core dump refused: HTTP ${status} $(json_field error < "${body}")" >&2
    exit 1
fi

mv "${body}" "${OUTPUT}"
bytes="$(wc -c < "${OUTPUT}" | tr -d ' ')"
echo "wrote ${OUTPUT} (${bytes} bytes)"

if [[ "${ELF}" == "-" ]]; then
    exit 0
fi
if [[ ! -f "${ELF}" ]]; then
    echo "no ELF at ${ELF} — the dump is downloaded; symbolicate it with:" >&2
    echo "  esp-coredump info_corefile --core ${OUTPUT} --core-format raw <slate.elf>" >&2
    exit 1
fi
if ! command -v esp-coredump >/dev/null 2>&1; then
    echo "esp-coredump is not on PATH — run \`. \$IDF_PATH/export.sh\` first. The dump is" \
         "downloaded and the command is:" >&2
    echo "  esp-coredump info_corefile --core ${OUTPUT} --core-format raw ${ELF}" >&2
    exit 1
fi

echo "symbolicating against $(basename "${ELF}")"
exec esp-coredump info_corefile --core "${OUTPUT}" --core-format raw "${ELF}"
