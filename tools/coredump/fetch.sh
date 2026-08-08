#!/usr/bin/env bash
#
# Fetch a core dump off a Slate panel and symbolicate it (design.md §11.3).
#
#   SLATE_TOKEN=... tools/coredump/fetch.sh <host> [elf] [output]
#
# The device token is handled exactly as tools/ota/upload.sh handles it: taken
# from $SLATE_TOKEN and handed to curl over a config file on stdin rather than as
# an argument, so it does not appear in the process list of a shared machine, and
# never printed. That script's header says how to read the token off a panel you
# have a cable to until #36 renders §4.3's pairing QR.
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
Fetch a core dump off a Slate panel and symbolicate it (design.md §11.3).

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
: "${SLATE_TOKEN:?set SLATE_TOKEN to the device token (design.md §4.3)}"

case "${HOST}" in
    http://*|https://*) BASE="${HOST}" ;;
    *)                  BASE="http://${HOST}" ;;
esac
API="${BASE}/api/v1"

authenticated_curl() {
    printf 'header = "Authorization: Bearer %s"\n' "${SLATE_TOKEN}" |
        curl --config - "$@"
}

# The status and the body are both needed: a 404 carries §4's
# {"error":"no_coredump"}, which is the answer to "has this panel ever crashed"
# and not a failure of this script. Written to the output path either way and
# removed unless the transfer succeeded, so a refusal cannot leave a previous
# dump behind looking like a fresh one.
set +e
status="$(authenticated_curl \
    --connect-timeout 5 \
    --max-time 120 \
    --output "${OUTPUT}" \
    --write-out '%{http_code}' \
    "${API}/coredump")"
curl_status=$?
set -e

if [[ ${curl_status} -ne 0 ]]; then
    rm -f "${OUTPUT}"
    echo "could not fetch from ${BASE}: curl exit ${curl_status}" >&2
    exit 1
fi
if [[ "${status}" == "404" ]]; then
    rm -f "${OUTPUT}"
    echo "no core dump on ${BASE} — nothing has panicked since the partition was last erased" >&2
    exit 1
fi
if [[ "${status}" != "200" ]]; then
    error="$(python3 -c 'import json,sys
try:
    print(json.load(open(sys.argv[1])).get("error", ""))
except Exception:
    pass' "${OUTPUT}" 2>/dev/null || true)"
    rm -f "${OUTPUT}"
    echo "core dump refused: HTTP ${status} ${error}" >&2
    exit 1
fi

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
