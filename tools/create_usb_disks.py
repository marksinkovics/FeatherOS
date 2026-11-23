#!/usr/bin/env python3
"""Builds the USB drives of the USB_STORAGE test in a folder.

usb-fat16.img is an MBR with a FAT16 and an ext2 partition, usb-floppy.img has
FAT16 on the whole drive, and usb-dirty.img has FAT16 on the whole drive with
FAT[1]'s clean-shutdown bit cleared, as a drive pulled out while mounted."""

import argparse
import struct
import subprocess
import sys
import tempfile
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", help="where the three images go")
    args = parser.parse_args()
    for path in Build(args.directory).values():
        print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
