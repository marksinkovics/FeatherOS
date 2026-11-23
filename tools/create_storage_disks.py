#!/usr/bin/env python3
"""Builds the physical-controller disk images of the NVME_TEST in a folder.

storage-controller.img is an MBR disk with four primary partitions, FAT16,
ext2, ext3 and ext4, and 1 MiB left free at the end for the kernel test hook's
raw transfers. BuildNamespaceDisks makes the three namespaces of the
NVME_NAMESPACES_TEST: a GPT disk in 4096-byte sectors, a FAT16 superfloppy and
an image for a namespace with 8 bytes of metadata per block."""

import argparse
import sys
import tempfile
from pathlib import Path

import create_test_disk as disks
import create_usb_disks as usb_disks

SECTOR_SIZE = disks.SECTOR_SIZE
CONTROLLER_IMAGE = "storage-controller.img"
FAT16_START = 2048
FAT16_SECTORS = 65536
EXT2_START = FAT16_START + FAT16_SECTORS
EXT2_SECTORS = 32768
EXT3_START = EXT2_START + EXT2_SECTORS
EXT3_SECTORS = 32768
EXT4_START = EXT3_START + EXT3_SECTORS
EXT4_SECTORS = 32768
FREE_SECTORS = 2048
CONTROLLER_SECTORS = EXT4_START + EXT4_SECTORS + FREE_SECTORS
FAT16_FILE = "NVME.TXT"
FAT16_CONTENTS = b"fat16 on nvme\n"
BIG_FILE = usb_disks.BIG_FILE
NAMESPACE_4K_IMAGE = "storage-ns4k.img"
NAMESPACE_512_IMAGE = "storage-ns512.img"
NAMESPACE_META_IMAGE = "storage-nsms.img"
NAMESPACE_512_SECTORS = 65536
NAMESPACE_512_FILE = "NS512.TXT"
NAMESPACE_512_CONTENTS = b"fat16 on a 512-byte namespace\n"
METADATA_BYTES = 8
NAMESPACE_META_BLOCKS = 2048


def BuildControllerDisk(directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / CONTROLLER_IMAGE
    with open(path, "wb") as image:
        image.truncate(CONTROLLER_SECTORS * SECTOR_SIZE)
        disks.write_mbr(
            image,
            disks.mbr_entry(
                disks.FAT16_PARTITION_TYPE, FAT16_START, FAT16_SECTORS
            ),
            disks.mbr_entry(
                disks.LINUX_PARTITION_TYPE, EXT2_START, EXT2_SECTORS
            ),
            disks.mbr_entry(
                disks.LINUX_PARTITION_TYPE, EXT3_START, EXT3_SECTORS
            ),
            disks.mbr_entry(
                disks.LINUX_PARTITION_TYPE, EXT4_START, EXT4_SECTORS
            ),
        )
    spec = f"{path}@@{FAT16_START * SECTOR_SIZE}"
    usb_disks.Mformat(spec, FAT16_START, FAT16_SECTORS, "NVMEFAT")
    with tempfile.TemporaryDirectory() as scratch:
        usb_disks.Mcopy(spec, FAT16_CONTENTS, FAT16_FILE, scratch)
        usb_disks.Mcopy(spec, usb_disks.BigContents(), BIG_FILE, scratch)
        for start, sectors, make in (
            (EXT2_START, EXT2_SECTORS, disks.make_ext2_volume),
            (EXT3_START, EXT3_SECTORS, disks.make_ext3_volume),
            (EXT4_START, EXT4_SECTORS, disks.make_data_volume),
        ):
            volume = Path(scratch) / f"volume-{start}.img"
            make(volume, sectors)
            disks.copy_volume(volume, path, start)
            volume.unlink()
    return path


def BuildNamespaceDisks(directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    four_k = directory / NAMESPACE_4K_IMAGE
    four_k.unlink(missing_ok=True)
    usb_disks.BuildFourK(directory).rename(four_k)
    flat = directory / NAMESPACE_512_IMAGE
    usb_disks.BuildWhole(
        flat,
        NAMESPACE_512_SECTORS,
        "NVMEFLOP",
        NAMESPACE_512_FILE,
        NAMESPACE_512_CONTENTS,
    )
    meta = directory / NAMESPACE_META_IMAGE
    with open(meta, "wb") as image:
        image.truncate(NAMESPACE_META_BLOCKS * (SECTOR_SIZE + METADATA_BYTES))
    return {"ns4k": four_k, "ns512": flat, "nsms": meta}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", help="where the image goes")
    args = parser.parse_args()
    print(BuildControllerDisk(args.directory))
    for path in BuildNamespaceDisks(args.directory).values():
        print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
