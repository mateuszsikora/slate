#!/usr/bin/env python3
"""Write the release manifest a panel checks for updates against (design.md §11.4).

    tools/ota/manifest.py --dist flasher/dist --version 1.0.0 \
                          --base-url https://mateuszsikora.github.io/slate/

Called by tools/flasher/build.sh, which has already assembled `dist` — the
browser flashing page, the images, and the ESP Web Tools manifest that names
their offsets. This adds one more file to that same directory:

    dist/ota/manifest.json

    {"version": "1.0.0", "board": "waveshare-s3-touch-7",
     "url": "https://…/firmware/1.0.0/slate.bin",
     "sha256": "…", "min_schema": 1}

**The page and the update channel are one deployment on purpose.** They serve
the same image to two audiences — a browser with a cable and a panel on a wall —
and a channel assembled by a different job, from a different build, is a channel
that can name an image the page does not have. Everything below is read out of
the artifacts rather than written down a second time: the checksum is of the
file that will be served, the version is read back out of that file's
application descriptor, and `board` and `min_schema` come from the firmware
headers that define them. The one thing this script is told rather than shown is
where the directory will be published, because nothing in a build knows that.

`ota/` rather than beside `manifest.json`, which is ESP Web Tools' own manifest
and a completely different document with the same natural name.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

# esp_app_desc_t sits immediately behind the 24-byte image and segment headers,
# and its `version` is the first 32 bytes of it. The release workflow reads the
# same offset back out of the linked image; see .github/workflows/release.yml.
APP_DESC_VERSION_OFFSET = 0x30
APP_DESC_VERSION_BYTES = 32

# The image header's first two bytes: ESP-IDF's magic, and the chip id that
# tools/flasher/manifest.py and tools/ota/upload.sh both refuse to publish or
# upload the wrong value of. 9 is ESP32-S3.
IMAGE_MAGIC = 0xE9
CHIP_ID_ESP32S3 = 9

# Where the constants that are not this script's to invent actually live.
BOARD_HEADER = Path("firmware/components/slate_api/include/slate_api.h")
BOARD_MACRO = "SLATE_API_MODEL_ID"
SCHEMA_HEADER = Path("firmware/components/slate_config/include/slate_config.h")
SCHEMA_MACRO = "SLATE_CONFIG_SCHEMA_MIN"


def fail(message: str) -> None:
    print(f"ota manifest: {message}", file=sys.stderr)
    sys.exit(1)


def read_string_macro(repo: Path, header: Path, macro: str) -> str:
    """The value of `#define <macro> "…"`, from the header that owns it."""
    text = (repo / header).read_text()
    match = re.search(rf'^#define\s+{macro}\s+"([^"]*)"', text, re.MULTILINE)
    if match is None:
        fail(f"no {macro} in {header}")
    return match.group(1)


def read_int_macro(repo: Path, header: Path, macro: str) -> int:
    text = (repo / header).read_text()
    match = re.search(rf"^#define\s+{macro}\s+(\d+)", text, re.MULTILINE)
    if match is None:
        fail(f"no {macro} in {header}")
    return int(match.group(1))


def image_version(image: Path) -> str:
    """The version the panel will report once this image is running."""
    blob = image.read_bytes()
    if len(blob) < APP_DESC_VERSION_OFFSET + APP_DESC_VERSION_BYTES:
        fail(f"{image} is too short to be an application image")
    if blob[0] != IMAGE_MAGIC or blob[12] != CHIP_ID_ESP32S3:
        fail(f"{image} is not an ESP32-S3 application image")
    field = blob[APP_DESC_VERSION_OFFSET : APP_DESC_VERSION_OFFSET + APP_DESC_VERSION_BYTES]
    return field.split(b"\0")[0].decode("utf-8", "replace")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dist", required=True, type=Path,
                        help="the assembled deployment directory, e.g. flasher/dist")
    parser.add_argument("--version", required=True,
                        help="the directory the images were published under: 1.0.0, no leading v")
    parser.add_argument("--base-url", required=True,
                        help="where --dist will be served from, e.g. https://user.github.io/slate/")
    parser.add_argument("--repo", default=".", type=Path, help="repository root")
    options = parser.parse_args()

    if options.version.startswith("v"):
        fail(f"version should not carry the tag's leading v: {options.version[1:]}")

    base_url = options.base_url
    if not base_url.startswith("https://"):
        # A panel refuses a manifest that names a plain-HTTP image, and would
        # refuse this one on arrival. Saying so here costs a tag rather than a
        # release nobody can install.
        fail(f"the channel must be https, not {base_url}")
    if not base_url.endswith("/"):
        base_url += "/"

    # The path tools/flasher/manifest.py copies the application image to. Read
    # out of the deployment rather than assumed: the file this hashes has to be
    # the file the URL below will serve.
    relative = f"firmware/{options.version}/slate.bin"
    image = options.dist / relative
    if not image.is_file():
        fail(f"no {image} — run tools/flasher/build.sh first")

    # The manifest's version is read out of the image, never taken from the
    # command line: a panel refuses an image whose application descriptor does
    # not say what the manifest promised, so a channel that disagrees with its
    # own binary is a channel nothing can install. --version names the directory
    # the images were published under, which is a different question and is
    # allowed to be a CI assembly's placeholder.
    version = image_version(image)
    if version != options.version:
        print(f"  note: the images are published under {options.version}, "
              f"and the image itself reports {version}")
    if re.fullmatch(r"\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?", version) is None:
        print(f"  note: {version} is not MAJOR.MINOR.PATCH — a panel will not offer it")

    manifest = {
        "version": version,
        "board": read_string_macro(options.repo, BOARD_HEADER, BOARD_MACRO),
        "url": base_url + relative,
        "sha256": hashlib.sha256(image.read_bytes()).hexdigest(),
        "min_schema": read_int_macro(options.repo, SCHEMA_HEADER, SCHEMA_MACRO),
    }

    out = options.dist / "ota" / "manifest.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(manifest, indent=2) + "\n")

    print(f"  update channel: {base_url}ota/manifest.json")
    print(f"    {manifest['version']}  {manifest['board']}  min_schema {manifest['min_schema']}")
    print(f"    sha256 {manifest['sha256']}")


if __name__ == "__main__":
    main()
