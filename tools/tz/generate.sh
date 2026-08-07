#!/usr/bin/env bash
#
# Generate the IANA -> POSIX TZ table compiled into the Slate firmware.
#
#   tools/tz/generate.sh [zoneinfo-directory]
#
# Output is firmware/components/slate_time/slate_time_zones.inc, and unlike the
# generated fonts it IS committed. The reasoning is the same one that gitignores
# those, applied to different numbers: the fonts are ~1.1 MB of hex per
# revision that nobody can review, this is ~600 lines of two strings each that a
# reviewer can read and a diff can show. Committing it also means the firmware
# builds on a machine with no tzdata, which the fonts already cost us once.
#
# Why the mapping exists at all: design.md §3.3 puts an IANA zone name in the
# configuration, and newlib's TZ variable takes a POSIX rule string. There is no
# zone database on the device — a panel that shipped one would have to be
# reflashed twice a year, which §11.4 is explicitly trying to avoid.
#
# Where the strings come from: every TZif v2+ file ends with a newline-delimited
# POSIX TZ string describing the rule currently in force, which is precisely the
# projection into the future the device needs. So this reads tzdata rather than
# reimplementing it, and the version it read is recorded in the output.
#
# Re-run it when the host's tzdata moves and the diff will show which zones
# changed their rules. That is a deliberate act with a reviewable result, which
# is the point.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
ZONEINFO="${1:-/usr/share/zoneinfo}"
OUT="${REPO_ROOT}/firmware/components/slate_time/slate_time_zones.inc"

if [[ ! -d "${ZONEINFO}" ]]; then
    echo "no zoneinfo at ${ZONEINFO}" >&2
    exit 1
fi

python3 - "${ZONEINFO}" "${OUT}" <<'PY'
import os, sys

zoneinfo, out_path = sys.argv[1], sys.argv[2]

version = "unknown"
version_file = os.path.join(zoneinfo, "+VERSION")
if os.path.exists(version_file):
    version = open(version_file).read().strip()


def posix_rule(path):
    """The POSIX TZ string in a TZif v2+ footer, or None."""
    with open(path, "rb") as f:
        data = f.read()
    # Magic and a version byte above '\0' — a v1-only file has no footer.
    if data[:4] != b"TZif" or data[4:5] == b"\0":
        return None
    # The footer is the last newline-delimited line of the file.
    end = data.rfind(b"\n")
    start = data.rfind(b"\n", 0, end)
    if start < 0 or end < 0:
        return None
    rule = data[start + 1:end].decode("ascii", "strict")
    return rule or None


zones = {}
for dirpath, dirnames, filenames in os.walk(zoneinfo):
    # Alternative encodings of the same zones. Neither is what newlib parses.
    dirnames[:] = [d for d in dirnames if d not in ("right", "posix")]
    for name in filenames:
        path = os.path.join(dirpath, name)
        zone = os.path.relpath(path, zoneinfo)
        try:
            rule = posix_rule(path)
        except (OSError, UnicodeDecodeError):
            continue
        if rule:
            zones[zone] = rule

if len(zones) < 100:
    sys.exit(f"only {len(zones)} zones found under {zoneinfo} — that is not a tz database")

with open(out_path, "w", encoding="ascii") as out:
    out.write("/*\n")
    out.write(" * IANA zone name -> POSIX TZ rule. GENERATED - do not edit.\n")
    out.write(" *\n")
    out.write(f" * tools/tz/generate.sh, from tzdata {version}.\n")
    out.write(" *\n")
    out.write(" * Sorted by zone name: slate_time.c binary-searches this table, and an\n")
    out.write(" * unsorted row would not fail to build, it would fail to be found.\n")
    out.write(" */\n")
    for zone in sorted(zones):
        out.write('{"%s", "%s"},\n' % (zone, zones[zone]))

print(f"{len(zones)} zones from tzdata {version} -> {out_path}")
PY
