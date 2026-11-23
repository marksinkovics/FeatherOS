#!/usr/bin/env python3
"""Builds the USB drives of the USB_STORAGE test in a folder.

usb-fat16.img is an MBR with a FAT16 and an ext2 partition, usb-floppy.img has
FAT16 on the whole drive, and usb-dirty.img has FAT16 on the whole drive with
FAT[1]'s clean-shutdown bit cleared, as a drive pulled out while mounted.

BuildLarge makes usb-large.img, a sparse 3 TiB GPT drive whose FAT16 partition
starts at 2.5 TiB (past what READ(10) can address), and BuildFourK makes
usb-4k.img, a GPT drive in 4096-byte sectors."""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path

import create_test_disk as disks

SECTOR_SIZE = disks.SECTOR_SIZE
FAT16_START = 2048
FAT16_SECTORS = 65536
EXT2_START = FAT16_START + FAT16_SECTORS
EXT2_SECTORS = 32768
FAT16_IMAGE_SECTORS = 131072
FLOPPY_SECTORS = 65536
DIRTY_SECTORS = 32768
CLUSTER_SECTORS = 4
SMALL_FILE = "USBFILE.TXT"
SMALL_CONTENTS = b"fat16 on a stick\n"
BIG_FILE = "BIG.BIN"
BIG_BYTES = 1024 * 1024
FLOPPY_FILE = "FLOPPY.TXT"
FLOPPY_CONTENTS = b"superfloppy\n"
DIRTY_FILE = "DIRTY.TXT"
DIRTY_CONTENTS = b"left dirty\n"
CLEAN_SHUTDOWN_BIT = 0x80
LARGE_IMAGE = "usb-large.img"
FOURK_IMAGE = "usb-4k.img"
LARGE_BYTES = 3 << 40
LARGE_SECTORS = LARGE_BYTES // SECTOR_SIZE
LARGE_FAT16_START = (5 << 39) // SECTOR_SIZE
LARGE_FAT16_SECTORS = 65536
LARGE_FILE = "LARGE.TXT"
LARGE_CONTENTS = b"fat16 past 2 TiB\n"
# A sparse image of 3 TiB takes a few MiB; anything near the FAT16 volume's
# 32 MiB means the host filesystem wrote the holes out.
LARGE_MAXIMUM_ALLOCATED = 128 << 20
FOURK_SECTOR_SIZE = 4096
FOURK_BYTES = 64 << 20
FOURK_SECTORS = FOURK_BYTES // FOURK_SECTOR_SIZE
FOURK_EXT4_START = (1 << 20) // FOURK_SECTOR_SIZE
FOURK_EXT4_SECTORS = (40 << 20) // FOURK_SECTOR_SIZE
FOURK_FAT16_START = FOURK_EXT4_START + FOURK_EXT4_SECTORS + 256
FOURK_FAT16_SECTORS = (16 << 20) // FOURK_SECTOR_SIZE
FOURK_FAT16_VOLUME_SECTORS = (16 << 20) // SECTOR_SIZE
# The partitions are entries 5 and 6: a 4096-byte sector holds 32 entries, so a
# reader that assumed 512-byte sectors would miss them.
FOURK_EMPTY_ENTRIES = (None,) * 4
FOURK_FILE = "FOURK.TXT"
FOURK_CONTENTS = b"ext4 on 4096-byte sectors\n"
FOURK_EXT4_UUID = "5f3c2a10-7d9e-4b61-8a47-2c1d9e0b6f13"
LARGE_PARTITION_GUID = uuid.UUID("6F2D7E52-3C8E-4D4B-9A51-7A0E5B1C2D21")
FOURK_EXT4_GUID = uuid.UUID("6F2D7E52-3C8E-4D4B-9A51-7A0E5B1C2D22")
FOURK_FAT16_GUID = uuid.UUID("6F2D7E52-3C8E-4D4B-9A51-7A0E5B1C2D23")
FAT16_IMAGE = "usb-fat16.img"
FLOPPY_IMAGE = "usb-floppy.img"
DIRTY_IMAGE = "usb-dirty.img"


def BigContents():
    """A pattern that differs between neighbouring clusters."""
    return bytes(
        (index * 7 + (index >> 11) * 13 + (index >> 16)) & 0xFF
        for index in range(BIG_BYTES)
    )


def Mformat(spec, start, sectors, label):
    subprocess.run(
        [
            "mformat",
            "-i",
            spec,
            "-H",
            str(start),
            "-T",
            str(sectors),
            # Left to choose, mformat may make FAT12 of a volume this small.
            "-c",
            str(CLUSTER_SECTORS),
            "-v",
            label,
            "::",
        ],
        check=True,
    )


def Mcopy(spec, contents, name, scratch):
    source = Path(scratch) / name
    source.write_bytes(contents)
    subprocess.run(["mcopy", "-i", spec, str(source), f"::{name}"], check=True)


def BuildPartitioned(path):
    with open(path, "wb") as image:
        image.truncate(FAT16_IMAGE_SECTORS * SECTOR_SIZE)
        disks.write_mbr(
            image,
            disks.mbr_entry(
                disks.FAT16_PARTITION_TYPE, FAT16_START, FAT16_SECTORS
            ),
            disks.mbr_entry(
                disks.LINUX_PARTITION_TYPE, EXT2_START, EXT2_SECTORS
            ),
        )
    spec = f"{path}@@{FAT16_START * SECTOR_SIZE}"
    Mformat(spec, FAT16_START, FAT16_SECTORS, "USBFAT")
    with tempfile.TemporaryDirectory() as scratch:
        Mcopy(spec, SMALL_CONTENTS, SMALL_FILE, scratch)
        Mcopy(spec, BigContents(), BIG_FILE, scratch)
        volume = Path(scratch) / "ext2.img"
        disks.make_ext2_volume(volume, EXT2_SECTORS)
        disks.copy_volume(volume, Path(path), EXT2_START)


def BuildWhole(path, sectors, label, name, contents):
    with open(path, "wb") as image:
        image.truncate(sectors * SECTOR_SIZE)
    Mformat(str(path), 0, sectors, label)
    with tempfile.TemporaryDirectory() as scratch:
        Mcopy(str(path), contents, name, scratch)


def ClearCleanShutdownBit(path):
    """FAT[1] bit 15, in every FAT copy."""
    with open(path, "r+b") as image:
        boot = image.read(SECTOR_SIZE)
        reserved = struct.unpack_from("<H", boot, 14)[0]
        copies = boot[16]
        fat_sectors = struct.unpack_from("<H", boot, 22)[0]
        for copy in range(copies):
            offset = (reserved + copy * fat_sectors) * SECTOR_SIZE + 3
            image.seek(offset)
            high = image.read(1)[0]
            image.seek(offset)
            image.write(bytes([high & ~CLEAN_SHUTDOWN_BIT & 0xFF]))


def Build(directory):
    """Writes the three images into directory; returns their paths by name."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    paths = {
        name: directory / name
        for name in (FAT16_IMAGE, FLOPPY_IMAGE, DIRTY_IMAGE)
    }
    BuildPartitioned(paths[FAT16_IMAGE])
    BuildWhole(
        paths[FLOPPY_IMAGE],
        FLOPPY_SECTORS,
        "USBFLOPPY",
        FLOPPY_FILE,
        FLOPPY_CONTENTS,
    )
    BuildWhole(
        paths[DIRTY_IMAGE],
        DIRTY_SECTORS,
        "USBDIRTY",
        DIRTY_FILE,
        DIRTY_CONTENTS,
    )
    ClearCleanShutdownBit(paths[DIRTY_IMAGE])
    return paths


class ImageUnavailable(Exception):
    pass


def AllocatedBytes(path):
    return os.stat(path).st_blocks * 512


def BuildLarge(directory):
    """Returns the path of usb-large.img, or raises ImageUnavailable when
    the host cannot hold it as a sparse file."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / LARGE_IMAGE
    path.unlink(missing_ok=True)
    try:
        with open(path, "wb") as image:
            image.truncate(LARGE_BYTES)
            disks.write_gpt(
                image,
                (
                    (
                        disks.BASIC_DATA_TYPE,
                        LARGE_PARTITION_GUID,
                        LARGE_FAT16_START,
                        LARGE_FAT16_START + LARGE_FAT16_SECTORS - 1,
                        "USBLARGE",
                    ),
                ),
                SECTOR_SIZE,
                LARGE_SECTORS,
            )
        with tempfile.TemporaryDirectory() as scratch:
            volume = Path(scratch) / "fat16.img"
            with open(volume, "wb") as image:
                image.truncate(LARGE_FAT16_SECTORS * SECTOR_SIZE)
            Mformat(str(volume), 0, LARGE_FAT16_SECTORS, "USBLARGE")
            Mcopy(str(volume), LARGE_CONTENTS, LARGE_FILE, scratch)
            Copy(volume, path, LARGE_FAT16_START * SECTOR_SIZE)
        allocated = AllocatedBytes(path)
    except OSError as error:
        path.unlink(missing_ok=True)
        raise ImageUnavailable(
            f"cannot create {LARGE_IMAGE}: {error}"
        ) from error
    if allocated > LARGE_MAXIMUM_ALLOCATED:
        path.unlink(missing_ok=True)
        raise ImageUnavailable(
            f"the host filesystem made {LARGE_IMAGE} {allocated >> 20} MiB "
            "on disk instead of a sparse file"
        )
    return path


def BuildFourK(directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / FOURK_IMAGE
    with open(path, "wb") as image:
        image.truncate(FOURK_BYTES)
        disks.write_gpt(
            image,
            (
                *FOURK_EMPTY_ENTRIES,
                (
                    disks.LINUX_FILESYSTEM_TYPE,
                    FOURK_EXT4_GUID,
                    FOURK_EXT4_START,
                    FOURK_EXT4_START + FOURK_EXT4_SECTORS - 1,
                    "USB4KEXT4",
                ),
                (
                    disks.BASIC_DATA_TYPE,
                    FOURK_FAT16_GUID,
                    FOURK_FAT16_START,
                    FOURK_FAT16_START + FOURK_FAT16_SECTORS - 1,
                    "USB4KFAT",
                ),
            ),
            FOURK_SECTOR_SIZE,
            FOURK_SECTORS,
        )
    with tempfile.TemporaryDirectory() as scratch:
        source = Path(scratch) / "tree"
        source.mkdir()
        (source / FOURK_FILE).write_bytes(FOURK_CONTENTS)
        volume = Path(scratch) / "ext4.img"
        disks.run_mke2fs(
            disks.find_mke2fs(),
            ("-t", "ext4", "-b", "4096", "-I", "256"),
            "USB4KEXT4",
            FOURK_EXT4_UUID,
            source,
            volume,
            FOURK_EXT4_SECTORS * FOURK_SECTOR_SIZE // SECTOR_SIZE,
            dict(os.environ),
            # A bare size counts blocks, not KiB, once -b is not 1024.
            "k",
        )
        Copy(volume, path, FOURK_EXT4_START * FOURK_SECTOR_SIZE)
        volume = Path(scratch) / "fat16.img"
        with open(volume, "wb") as image:
            image.truncate(FOURK_FAT16_VOLUME_SECTORS * SECTOR_SIZE)
        Mformat(str(volume), 0, FOURK_FAT16_VOLUME_SECTORS, "USB4KFAT")
        Copy(volume, path, FOURK_FAT16_START * FOURK_SECTOR_SIZE)
    return path


def Copy(volume, disk_path, offset):
    with open(volume, "rb") as source, open(disk_path, "r+b") as disk:
        disk.seek(offset)
        shutil.copyfileobj(source, disk)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", help="where the images go")
    parser.add_argument(
        "--large",
        action="store_true",
        help="also make usb-large.img and usb-4k.img",
    )
    args = parser.parse_args()
    for path in Build(args.directory).values():
        print(path)
    if args.large:
        print(BuildFourK(args.directory))
        try:
            print(BuildLarge(args.directory))
        except ImageUnavailable as error:
            print(f"create_usb_disks.py: {error}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
