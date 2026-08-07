#!/usr/bin/env bash
#
# Build, flash and capture one measurement run.
#
#   tools/s2_run.sh <name> [-D... ...]
#
# Every run gets its own build directory *and* its own sdkconfig, because
# SDKCONFIG_DEFAULTS is consulted only when no sdkconfig exists yet — the trap
# that silently produced two identical "variants" in S-3.
set -euo pipefail

NAME="$1"; shift
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${DIR}/build-${NAME}"
PORT="${S2_PORT:-/dev/cu.usbmodem5C372489711}"
LOG="${S2_LOGDIR:-/tmp}/s2_${NAME}.log"

idf.py -C "${DIR}" -B "${BUILD}" \
    -DSDKCONFIG="${BUILD}/sdkconfig" \
    -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.refr10" \
    "$@" build > "${BUILD}.buildlog" 2>&1

idf.py -C "${DIR}" -B "${BUILD}" -p "${PORT}" flash > "${BUILD}.flashlog" 2>&1

python3 - "${PORT}" "${LOG}" <<'PY'
import serial, sys, time
port, log = sys.argv[1], sys.argv[2]
s = serial.Serial(port, 115200, timeout=1)
s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False)
buf, end = b'', time.time() + 110
while time.time() < end:
    d = s.read(8192)
    if d:
        buf += d
        if b'S2END' in buf or b'S2FAIL' in buf:
            break
open(log, 'wb').write(buf)
t = buf.decode('utf-8', 'replace')
print('FAIL' if 'S2FAIL' in t else 'ok', log, len(t.splitlines()), 'lines')
PY
