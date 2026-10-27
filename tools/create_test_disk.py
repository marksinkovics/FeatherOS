#!/usr/bin/env python3

import argparse
import struct
import subprocess
import tempfile
import uuid
import zlib
from pathlib import Path

DISK_SIZE = 32 * 1024 * 1024
SECTOR_SIZE = 512
DISK_SECTORS = DISK_SIZE // SECTOR_SIZE
PARTITION_START = 2048
FAT16_PARTITION_TYPE = 0x06
PROTECTIVE_PARTITION_TYPE = 0xEE
GPT_ENTRY_COUNT = 128
GPT_ENTRY_SIZE = 128
# Partition entries take 32 sectors; with the header, each copy of the table
# needs 33.
GPT_TABLE_SECTORS = 1 + GPT_ENTRY_COUNT * GPT_ENTRY_SIZE // SECTOR_SIZE
BASIC_DATA_TYPE = uuid.UUID("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7")
# Fixed GUIDs keep the image reproducible.
DISK_GUID = uuid.UUID("6F2D7E52-3C8E-4D4B-9A51-7A0E5B1C2D01")
PARTITION_GUID = uuid.UUID("6F2D7E52-3C8E-4D4B-9A51-7A0E5B1C2D02")
# mtools writes this file with a VFAT long name, which the kernel must read.
LONG_NAME_FILE = "Read Me First.txt"
LONG_NAME_CONTENTS = b"Long names from mtools work.\n"


def mbr_entry(partition_type, first_sector, sector_count):
    entry = bytearray(16)
    entry[1:4] = bytes((0, 2, 0))
    entry[4] = partition_type
    entry[5:8] = bytes((0xFE, 0xFF, 0xFF))
    struct.pack_into("<II", entry, 8, first_sector, sector_count)
    return entry


def write_mbr(disk_file, entry):
    mbr = bytearray(SECTOR_SIZE)
    mbr[446:462] = entry
    mbr[510:512] = b"\x55\xaa"
    disk_file.seek(0)
    disk_file.write(mbr)


def gpt_header(current, backup, entries_sector, entries_crc):
    header = bytearray(SECTOR_SIZE)
    struct.pack_into(
        "<8sIIIIQQQQ16sQIII",
        header,
        0,
        b"EFI PART",
        0x00010000,
        92,
        0,
        0,
        current,
        backup,
        GPT_TABLE_SECTORS + 1,
        DISK_SECTORS - GPT_TABLE_SECTORS - 1,
        DISK_GUID.bytes_le,
        entries_sector,
        GPT_ENTRY_COUNT,
        GPT_ENTRY_SIZE,
        entries_crc,
    )
    # The header CRC covers the header with its own CRC field zeroed.
    struct.pack_into("<I", header, 16, zlib.crc32(header[:92]))
    return header


def write_gpt(disk_file, first_sector, last_sector):
    entries = bytearray(GPT_ENTRY_COUNT * GPT_ENTRY_SIZE)
    struct.pack_into(
        "<16s16sQQQ72s",
        entries,
        0,
        BASIC_DATA_TYPE.bytes_le,
        PARTITION_GUID.bytes_le,
        first_sector,
        last_sector,
        0,
        "FEATHEROS".encode("utf-16-le"),
    )
    entries_crc = zlib.crc32(entries)
    backup_header = DISK_SECTORS - 1
    backup_entries = DISK_SECTORS - GPT_TABLE_SECTORS
    write_mbr(
        disk_file, mbr_entry(PROTECTIVE_PARTITION_TYPE, 1, DISK_SECTORS - 1)
    )
    for header_sector, entries_sector, other in (
        (1, 2, backup_header),
        (backup_header, backup_entries, 1),
    ):
        disk_file.seek(entries_sector * SECTOR_SIZE)
        disk_file.write(entries)
        disk_file.seek(header_sector * SECTOR_SIZE)
        disk_file.write(
            gpt_header(header_sector, other, entries_sector, entries_crc)
        )


def main():
    parser = argparse.ArgumentParser(
        description="Create a small MBR or GPT disk with one FAT16 partition."
    )
    parser.add_argument("image", help="Output raw disk image")
    parser.add_argument(
        "file", help="File to copy into the FAT16 root directory"
    )
    parser.add_argument(
        "--layout",
        choices=("mbr", "gpt"),
        default="mbr",
        help="Partition table type",
    )
    args = parser.parse_args()

    # GPT keeps a backup table in the disk's last sectors, so its partition
    # stops short of the end.
    last_sector = (
        DISK_SECTORS - GPT_TABLE_SECTORS - 1
        if args.layout == "gpt"
        else DISK_SECTORS - 1
    )
    partition_sectors = last_sector - PARTITION_START + 1

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
            "-T",
            str(partition_sectors),
            "-v",
            "FEATHEROS",
            "::",
        ],
        check=True,
    )
    subprocess.run(
        ["mcopy", "-i", image_spec, args.file, "::BLOCK.TXT"], check=True
    )
    # The shell starts in /home, and PATH ends with /bin.
    subprocess.run(["mmd", "-i", image_spec, "::home", "::bin"], check=True)
    with tempfile.NamedTemporaryFile() as long_name_source:
        long_name_source.write(LONG_NAME_CONTENTS)
        long_name_source.flush()
        subprocess.run(
            [
                "mcopy",
                "-i",
                image_spec,
                long_name_source.name,
                f"::{LONG_NAME_FILE}",
            ],
            check=True,
        )

    with image.open("r+b") as disk_file:
        if args.layout == "gpt":
            write_gpt(disk_file, PARTITION_START, last_sector)
        else:
            write_mbr(
                disk_file,
                mbr_entry(
                    FAT16_PARTITION_TYPE, PARTITION_START, partition_sectors
                ),
            )


if __name__ == "__main__":
    main()
