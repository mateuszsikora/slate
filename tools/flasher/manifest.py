#!/usr/bin/env python3
"""Assemble the ESP Web Tools manifest and the images it flashes (design.md §9.1).

    tools/flasher/manifest.py --build-dir firmware/build-release \
                              --version 1.0.0 --out flasher/dist

Called by tools/flasher/build.sh, which does the vendoring around it. Kept
separate because this half is where the offsets are decided, and the offsets are
the part that cannot be wrong.

**Everything is read out of the build, nothing is written down twice.** ESP-IDF
generates `flash_project_args` in the build directory — the same file
`idf.py flash` uses — and it names each image and the address it belongs at. A
flasher that carried its own copy of those four numbers would be a second
partition table: firmware/partitions.csv cannot be changed without a serial
flash (§6.3), and the failure mode of a stale copy here is a panel that takes an
image and then does not boot, on a machine whose owner has no ESP-IDF to check
with.

Reading them is not by itself enough, because a build directory that was
configured against a different partition table would produce a self-consistent
set of wrong numbers. So the two addresses that matter are checked back against
the committed table before anything is copied, and the application image is
checked for the ESP32-S3 header the same way tools/ota/upload.sh checks it —
this is the one place where the wrong board's image would be published under
the right board's name.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import shutil
import sys
from pathlib import Path

CHIP_FAMILY = "ESP32-S3"

# The partition names whose offsets this script re-derives from partitions.csv.
# Keyed by the image ESP-IDF flashes there; anything else in flash_project_args
# is copied at the offset the build gives without a second opinion, because
# nothing else in the table is addressed by name.
CHECKED_AGAINST_TABLE = {
    "slate.bin": "ota_0",
    "ota_data_initial.bin": "otadata",
}


def fail(message: str) -> None:
    print(f"flasher: {message}", file=sys.stderr)
    sys.exit(1)


def read_flash_args(build_dir: Path) -> list[tuple[int, Path]]:
    """The (offset, image) pairs ESP-IDF wrote for `idf.py flash`."""
    args_file = build_dir / "flash_project_args"
    if not args_file.is_file():
        fail(f"no {args_file} — run `idf.py -B {build_dir.name} build` first")

    parts: list[tuple[int, Path]] = []
    for line in args_file.read_text().splitlines():
        line = line.strip()
        # The first line carries --flash_mode and friends. Those belong to
        # esptool, and ESP Web Tools sets its own from the image header.
        if not line or line.startswith("-"):
            continue
        offset, _, relative = line.partition(" ")
        image = build_dir / relative.strip()
        if not image.is_file():
            fail(f"{args_file} names {relative.strip()}, which does not exist")
        parts.append((int(offset, 0), image))

    if not parts:
        fail(f"{args_file} names no images")
    return sorted(parts)


def read_partition_table(repo: Path) -> dict[str, int]:
    """Offsets from the committed table, which is the authority §6.3 settled."""
    offsets: dict[str, int] = {}
    with (repo / "firmware" / "partitions.csv").open() as table:
        for row in csv.reader(table):
            if not row or row[0].strip().startswith("#"):
                continue
            offsets[row[0].strip()] = int(row[3].strip(), 0)
    return offsets


def partition_table_offset(build_dir: Path) -> int | None:
    """Where this build puts the table itself; it is not in the table."""
    sdkconfig = build_dir / "sdkconfig"
    if not sdkconfig.is_file():
        return None
    match = re.search(
        r"^CONFIG_PARTITION_TABLE_OFFSET=(0x[0-9a-fA-F]+|\d+)$",
        sdkconfig.read_text(),
        re.MULTILINE,
    )
    return int(match.group(1), 0) if match else None


def check_application_header(image: Path) -> None:
    """0xE9 and chip id 0x09, as tools/ota/upload.sh checks before uploading.

    An ESP32 image also starts with 0xE9, so the magic alone would let the
    wrong board's firmware be published as this board's.
    """
    header = image.read_bytes()[:14]
    if len(header) < 14:
        fail(f"{image} is too short to be an application image")
    if header[0] != 0xE9:
        fail(f"{image} is not an ESP application image (no 0xE9 magic)")
    if header[12] != 0x09:
        fail(f"{image} is not an ESP32-S3 image (chip id {header[12]:#04x}) — wrong board?")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument(
        "--repo",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="repository root, for firmware/partitions.csv",
    )
    options = parser.parse_args()

    build_dir = options.build_dir.resolve()
    parts = read_flash_args(build_dir)
    table = read_partition_table(options.repo)
    table_offset = partition_table_offset(build_dir)

    for offset, image in parts:
        expected_partition = CHECKED_AGAINST_TABLE.get(image.name)
        if expected_partition is not None:
            expected = table.get(expected_partition)
            if expected is None:
                fail(f"firmware/partitions.csv has no {expected_partition} partition")
            if offset != expected:
                fail(
                    f"{image.name} would be written at {offset:#x}, but "
                    f"partitions.csv puts {expected_partition} at {expected:#x} — "
                    "this build was configured against a different table"
                )
        if image.name == "partition-table.bin" and table_offset is not None and offset != table_offset:
            fail(
                f"partition-table.bin would be written at {offset:#x}, but this "
                f"build's CONFIG_PARTITION_TABLE_OFFSET is {table_offset:#x}"
            )
        if image.name == "slate.bin":
            check_application_header(image)

    if len({image.name for _, image in parts}) != len(parts):
        fail("two images share a file name and would overwrite each other")

    # Images live under the version they belong to. The manifest is fetched by a
    # browser and so are the images beside it, and a cache holding last
    # release's `slate.bin` under a path that no longer means the same thing is
    # a panel flashed with the wrong firmware and no way to tell from the page.
    images_dir = options.out / "firmware" / options.version
    images_dir.mkdir(parents=True, exist_ok=True)

    manifest_parts = []
    for offset, image in parts:
        shutil.copy2(image, images_dir / image.name)
        manifest_parts.append(
            {"path": f"firmware/{options.version}/{image.name}", "offset": offset}
        )
        print(f"  {offset:#09x}  {image.name}  ({image.stat().st_size} B)")

    manifest = {
        "name": "Slate",
        "version": options.version,
        # ESP Web Tools offers the erase as a checkbox on a first install. It is
        # offered rather than forced because erasing takes NVS and LittleFS with
        # it — the device token, the WiFi credentials and the dashboard (§6.3)
        # — and a panel being reflashed by hand is usually one whose owner
        # wants those kept. The page says so next to the button.
        "new_install_prompt_erase": True,
        # Slate provisions WiFi over the setup access point of §9, not over
        # Improv serial. Zero is what stops the dialog waiting for a handshake
        # this firmware will never answer, and sends people to the screen, where
        # the flow they are about to follow is already printed.
        "new_install_improv_wait_time": 0,
        "builds": [{"chipFamily": CHIP_FAMILY, "parts": manifest_parts}],
    }
    (options.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
