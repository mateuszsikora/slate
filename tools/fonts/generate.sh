#!/usr/bin/env bash
#
# Generate the LVGL fonts compiled into the Slate firmware.
#
# Run from anywhere; paths are resolved relative to this script. Requires node
# (lv_font_conv is fetched through npx) and network access on first run.
#
#   tools/fonts/generate.sh
#
# Output lands in firmware/components/slate_theme/assets/ as LVGL C sources and
# an icon-name header. Those are NOT committed: roughly 1.1 MB of generated data
# per revision that nobody can review and that would re-enter git history on
# every glyph-list change. This script and icons.txt are the sources of truth;
# run it before building the firmware.
#
# The firmware project itself arrives in M1 — this script creates its output
# directory, so it runs standalone until then.
#
# Why these parameters — all from design.md §8:
#   - Three type steps: hero 44 px, body 20 px, caption 15 px.
#   - bpp 4. Without antialiasing everything looks dated regardless of the rest.
#   - Coverage: Latin-1 plus Polish diacritics.
#
# Two icon sizes rather than one: §7.1 specifies a "large icon" for the 2x2
# light tile and a normal one for 1x1, and an icon font scaled by LVGL looks
# poor. Each size is emitted separately so its cost can be read off and dropped
# if the flash budget ever demands it.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
OUT_DIR="${REPO_ROOT}/firmware/components/slate_theme/assets"
WORK_DIR="${SCRIPT_DIR}/.work"

# Pinned upstream sources. Bumping either of these changes the image size, so
# the versions belong in docs/spikes/s3.md alongside the measurement.
INTER_VERSION="4.0"
INTER_URL="https://github.com/rsms/inter/releases/download/v${INTER_VERSION}/Inter-${INTER_VERSION}.zip"
INTER_ZIP_SHA256="ff970a5d4561a04f102a7cb781adbd6ac4e9b6c460914c7a101f15acb7f7d1a4"
INTER_TTF_SHA256="64f8be6e55c37e32ef03da99714bf3aa58b8f2099bfe4f759a7578e3b8291123"
MDI_VERSION="7.4.47"
MDI_TTF_URL="https://raw.githubusercontent.com/Templarian/MaterialDesign-Webfont/v${MDI_VERSION}/fonts/materialdesignicons-webfont.ttf"
MDI_CSS_URL="https://raw.githubusercontent.com/Templarian/MaterialDesign-Webfont/v${MDI_VERSION}/css/materialdesignicons.css"
MDI_TTF_SHA256="61e8aba5a4e981fe22cf7c8e8bcdbea00476e75c62c37f01bf7ee33361d68428"
MDI_CSS_SHA256="cd66a937a12b4368031fb980d57346803260c6cfa5e737bbad526aed2c5cda0e"

# Latin-1 printable plus the Polish letters that fall outside it.
# 0x104-0x107 Ąą Ćć | 0x118-0x119 Ęę | 0x141-0x144 Łł Ńń
# 0x15A-0x15B Śś    | 0x179-0x17C Źź Żż
TEXT_RANGES="-r 0x20-0xFF -r 0x104-0x107 -r 0x118-0x119 -r 0x141-0x144 -r 0x15A-0x15B -r 0x179-0x17C"

mkdir -p "${WORK_DIR}" "${OUT_DIR}"

# --- Fetch sources ----------------------------------------------------------

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

verify_file() {
    local path="$1" expected="$2"
    [[ -f "${path}" ]] && [[ "$(sha256_file "${path}")" == "${expected}" ]]
}

fetch_file() {
    local url="$1" path="$2" expected="$3"
    if verify_file "${path}" "${expected}"; then
        return
    fi

    if [[ -f "${path}" ]]; then
        echo "==> replacing ${path##*/}: cached checksum does not match" >&2
        rm -f "${path}"
    fi

    local partial="${path}.download"
    rm -f "${partial}"
    curl --fail --silent --show-error --location --retry 3 -o "${partial}" "${url}"
    if ! verify_file "${partial}" "${expected}"; then
        echo "checksum mismatch for ${url}" >&2
        rm -f "${partial}"
        return 1
    fi
    mv "${partial}" "${path}"
}

echo "==> verifying Inter ${INTER_VERSION}"
fetch_file "${INTER_URL}" "${WORK_DIR}/inter.zip" "${INTER_ZIP_SHA256}"
if ! verify_file "${WORK_DIR}/Inter-Regular.ttf" "${INTER_TTF_SHA256}"; then
    unzip -o -j -q "${WORK_DIR}/inter.zip" "extras/ttf/Inter-Regular.ttf" -d "${WORK_DIR}"
fi
if ! verify_file "${WORK_DIR}/Inter-Regular.ttf" "${INTER_TTF_SHA256}"; then
    echo "checksum mismatch for Inter-Regular.ttf extracted from the pinned archive" >&2
    exit 1
fi

echo "==> verifying Material Design Icons ${MDI_VERSION}"
fetch_file "${MDI_TTF_URL}" "${WORK_DIR}/mdi.ttf" "${MDI_TTF_SHA256}"
fetch_file "${MDI_CSS_URL}" "${WORK_DIR}/mdi.css" "${MDI_CSS_SHA256}"

# --- Resolve icon names to codepoints ---------------------------------------
#
# The CSS is the authoritative name->codepoint map for a given MDI release.
# A name that no longer exists upstream is a hard error: silently dropping it
# would leave a component rendering a blank box at runtime.

ICON_ARGS="$(python3 - "${SCRIPT_DIR}/icons.txt" "${WORK_DIR}/mdi.css" \
    "${OUT_DIR}/slate_icons.h" <<'PY'
import re, sys

names_path, css_path, header_path = sys.argv[1:]

wanted = []
for line in open(names_path, encoding="utf-8"):
    line = line.split("#", 1)[0].strip()
    if line:
        wanted.append(line)

css = open(css_path, encoding="utf-8").read()
mapping = dict(re.findall(r'\.mdi-([a-z0-9-]+)::before\s*\{\s*content:\s*"\\([0-9A-Fa-f]+)"', css))

missing = [n for n in wanted if n not in mapping]
if missing:
    sys.exit("icons not found in this MDI release: " + ", ".join(missing))

seen, args, icons = set(), [], []
for n in wanted:
    cp = mapping[n].upper()
    if cp in seen:
        sys.exit(f"duplicate codepoint for {n}")
    seen.add(cp)
    args.append(f"-r 0x{cp}")
    icons.append((n, cp))

with open(header_path, "w", encoding="utf-8") as header:
    header.write("/* Generated by tools/fonts/generate.sh from pinned MDI names. */\n")
    header.write("#pragma once\n\n")
    for name, cp in icons:
        symbol = name.upper().replace("-", "_")
        utf8 = "".join(f"\\x{byte:02X}" for byte in chr(int(cp, 16)).encode("utf-8"))
        header.write(f"#define SLATE_ICON_{symbol} \"{utf8}\"\n")
        header.write(f"#define SLATE_ICON_CODE_{symbol} 0x{cp}\n")

print(" ".join(args))
print(f"resolved {len(args)} icons", file=sys.stderr)
PY
)"

# --- Generate ---------------------------------------------------------------

gen_text() {
    local size="$1" name="$2"
    echo "==> ${name} (Inter ${size} px, bpp 4)"
    npx --yes lv_font_conv@1.5.3 \
        --font "${WORK_DIR}/Inter-Regular.ttf" \
        ${TEXT_RANGES} \
        --size "${size}" --bpp 4 --no-compress \
        --format lvgl --lv-include lvgl.h \
        -o "${OUT_DIR}/${name}.c"
}

gen_icons() {
    local size="$1" name="$2"
    echo "==> ${name} (MDI ${size} px, bpp 4)"
    npx --yes lv_font_conv@1.5.3 \
        --font "${WORK_DIR}/mdi.ttf" \
        ${ICON_ARGS} \
        --size "${size}" --bpp 4 --no-compress \
        --format lvgl --lv-include lvgl.h \
        -o "${OUT_DIR}/${name}.c"
}

gen_text 44 slate_font_hero_44
gen_text 20 slate_font_body_20
gen_text 15 slate_font_caption_15
gen_icons 28 slate_font_icons_28
gen_icons 48 slate_font_icons_48

echo
echo "==> generated sizes"
ls -l "${OUT_DIR}"
