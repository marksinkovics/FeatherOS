#!/usr/bin/env python3

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid
import zlib
from pathlib import Path

DISK_SIZE = 48 * 1024 * 1024
SECTOR_SIZE = 512
DISK_SECTORS = DISK_SIZE // SECTOR_SIZE
PARTITION_START = 2048
# The FAT16 partition ends where the ext2 one starts, at 32 MiB.
EXT2_START = 65536
FAT16_PARTITION_TYPE = 0x06
LINUX_PARTITION_TYPE = 0x83
PROTECTIVE_PARTITION_TYPE = 0xEE
GPT_ENTRY_COUNT = 128
GPT_ENTRY_SIZE = 128
# Partition entries take 32 sectors; with the header, each copy of the table
# needs 33.
GPT_TABLE_SECTORS = 1 + GPT_ENTRY_COUNT * GPT_ENTRY_SIZE // SECTOR_SIZE
BASIC_DATA_TYPE = uuid.UUID("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7")
LINUX_FILESYSTEM_TYPE = uuid.UUID("0FC63DAF-8483-4772-8E79-3D69D8477DE4")
# Fixed GUIDs and times keep the image reproducible.
DISK_GUID = uuid.UUID("6F2D7E52-3C8E-4D4B-9A51-7A0E5B1C2D01")
PARTITION_GUID = uuid.UUID("6F2D7E52-3C8E-4D4B-9A51-7A0E5B1C2D02")
EXT2_PARTITION_GUID = uuid.UUID("6F2D7E52-3C8E-4D4B-9A51-7A0E5B1C2D03")
EXT2_VOLUME_UUID = "6f2d7e52-3c8e-4d4b-9a51-7a0e5b1c2d04"
EXT2_TIME = 1767225600
# mtools writes this file with a VFAT long name, which the kernel must read.
LONG_NAME_FILE = "Read Me First.txt"
LONG_NAME_CONTENTS = b"Long names from mtools work.\n"
# The ext2 partition's files, which the STORAGE kernel test and the shell
# smoke test read. big.txt is past 64 KiB, so it needs an indirect block.
EXT2_FILES = {
    "hello.txt": b"Hello from the ext2 partition.\n",
    "big.txt": b"".join(b"ext2 line %05d\n" % line for line in range(5000)),
    "docs/notes/deep.txt": b"nested ext2 file\n",
    "a-file-name-longer-than-sixty-three-characters-that-fat16-cannot-hold"
    ".txt": b"long ext2 name\n",
}
# Under 60 bytes a link's target lives in its inode; a longer one takes a
# block of its own.
EXT2_LINKS = {
    "fast-link": "hello.txt",
    "slow-link": "docs/notes/" + "../notes/" * 5 + "deep.txt",
}
E2FSPROGS_DIRECTORIES = (
    "/opt/homebrew/opt/e2fsprogs/sbin",
    "/usr/local/opt/e2fsprogs/sbin",
)


def mbr_entry(partition_type, first_sector, sector_count):
    entry = bytearray(16)
    entry[1:4] = bytes((0, 2, 0))
    entry[4] = partition_type
    entry[5:8] = bytes((0xFE, 0xFF, 0xFF))
    struct.pack_into("<II", entry, 8, first_sector, sector_count)
    return entry


def write_mbr(disk_file, *entries):
    mbr = bytearray(SECTOR_SIZE)
    for index, entry in enumerate(entries):
        mbr[446 + 16 * index : 462 + 16 * index] = entry
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


def write_gpt(disk_file, partitions):
    entries = bytearray(GPT_ENTRY_COUNT * GPT_ENTRY_SIZE)
    for index, (type_guid, guid, first, last, name) in enumerate(partitions):
        struct.pack_into(
            "<16s16sQQQ72s",
            entries,
            index * GPT_ENTRY_SIZE,
            type_guid.bytes_le,
            guid.bytes_le,
            first,
            last,
            0,
            name.encode("utf-16-le"),
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


def find_mke2fs():
    """Homebrew's keg-only e2fsprogs first, as src/fs/core's CMake looks."""
    for directory in E2FSPROGS_DIRECTORIES:
        candidate = Path(directory) / "mke2fs"
        if os.access(candidate, os.X_OK):
            return str(candidate)
    found = shutil.which("mke2fs") or shutil.which(
        "mke2fs", path="/usr/sbin:/sbin"
    )
    if found is None:
        sys.exit(
            "create_test_disk.py: mke2fs not found; install e2fsprogs "
            "(brew install e2fsprogs, or apt install e2fsprogs)"
        )
    return found


def make_ext2_volume(volume_path, sector_count):
    with tempfile.TemporaryDirectory() as source_name:
        source = Path(source_name)
        for name, contents in EXT2_FILES.items():
            path = source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(contents)
        for name, target in EXT2_LINKS.items():
            (source / name).symlink_to(target)
        for path in sorted(source.rglob("*"), reverse=True):
            os.utime(path, (EXT2_TIME, EXT2_TIME), follow_symlinks=False)
        environment = dict(os.environ, E2FSPROGS_FAKE_TIME=str(EXT2_TIME))
        # mke2fs warns about 128-byte inodes even with -q, so its output is
        # shown only when it fails.
        result = subprocess.run(
            [
                find_mke2fs(),
                "-q",
                "-F",
                "-t",
                "ext2",
                "-b",
                "1024",
                "-I",
                "128",
                "-L",
                "FEATHEREXT2",
                "-U",
                EXT2_VOLUME_UUID,
                "-E",
                f"hash_seed={EXT2_VOLUME_UUID},root_owner=0:0",
                "-d",
                str(source),
                str(volume_path),
                str(sector_count * SECTOR_SIZE // 1024),
            ],
            capture_output=True,
            text=True,
            env=environment,
            check=False,
        )
    if result.returncode != 0:
        sys.exit(
            f"create_test_disk.py: mke2fs failed:\n{result.stdout}"
            f"{result.stderr}"
        )


def main():
    parser = argparse.ArgumentParser(
        description="Create a small MBR or GPT disk with a FAT16 partition "
        "and an ext2 partition after it."
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

    # GPT keeps a backup table in the disk's last sectors, so the last
    # partition stops short of the end.
    last_sector = (
        DISK_SECTORS - GPT_TABLE_SECTORS - 1
        if args.layout == "gpt"
        else DISK_SECTORS - 1
    )
    partition_sectors = EXT2_START - PARTITION_START
    ext2_sectors = last_sector - EXT2_START + 1

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

    with tempfile.TemporaryDirectory() as work:
        volume = Path(work) / "ext2.img"
        make_ext2_volume(volume, ext2_sectors)
        with volume.open("rb") as volume_file, image.open("r+b") as disk_file:
            disk_file.seek(EXT2_START * SECTOR_SIZE)
            shutil.copyfileobj(volume_file, disk_file)

    with image.open("r+b") as disk_file:
        if args.layout == "gpt":
            write_gpt(
                disk_file,
                (
                    (
                        BASIC_DATA_TYPE,
                        PARTITION_GUID,
                        PARTITION_START,
                        EXT2_START - 1,
                        "FEATHEROS",
                    ),
                    (
                        LINUX_FILESYSTEM_TYPE,
                        EXT2_PARTITION_GUID,
                        EXT2_START,
                        last_sector,
                        "FEATHEREXT2",
                    ),
                ),
            )
        else:
            write_mbr(
                disk_file,
                mbr_entry(
                    FAT16_PARTITION_TYPE, PARTITION_START, partition_sectors
                ),
                mbr_entry(LINUX_PARTITION_TYPE, EXT2_START, ext2_sectors),
            )


if __name__ == "__main__":
    main()
