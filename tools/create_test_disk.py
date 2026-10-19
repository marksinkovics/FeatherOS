#!/usr/bin/env python3

import argparse
import struct
import subprocess
from pathlib import Path

DISK_SIZE = 32 * 1024 * 1024
SECTOR_SIZE = 512
PARTITION_START = 2048
FAT16_PARTITION_TYPE = 0x06


def main():
    parser = argparse.ArgumentParser(
        description="Create a small MBR/FAT16 test disk."
    )
    parser.add_argument("image", help="Output raw disk image")
    parser.add_argument(
        "file", help="File to copy into the FAT16 root directory"
    )
    args = parser.parse_args()

    image = Path(args.image).resolve()
    image.parent.mkdir(parents=True, exist_ok=True)
    with image.open("wb") as disk_file:
        disk_file.truncate(DISK_SIZE)

    image_spec = f"{image}@@{PARTITION_START * SECTOR_SIZE}"
    subprocess.run(
        [
            "mformat",
            "-i",
            image_spec,
            "-H",
            str(PARTITION_START),
            "-v",
            "FEATHEROS",
            "::",
        ],
        check=True,
    )
    subprocess.run(
        ["mcopy", "-i", image_spec, args.file, "::BLOCK.TXT"], check=True
    )

    partition_count = DISK_SIZE // SECTOR_SIZE - PARTITION_START
    mbr = bytearray(SECTOR_SIZE)
    entry_offset = 446
    mbr[entry_offset] = 0
    mbr[entry_offset + 1 : entry_offset + 4] = bytes((0, 2, 0))
    mbr[entry_offset + 4] = FAT16_PARTITION_TYPE
    mbr[entry_offset + 5 : entry_offset + 8] = bytes((0xFE, 0xFF, 0xFF))
    struct.pack_into(
        "<II", mbr, entry_offset + 8, PARTITION_START, partition_count
    )
    mbr[510:512] = b"\x55\xaa"
    with image.open("r+b") as disk_file:
        disk_file.write(mbr)


if __name__ == "__main__":
    main()
