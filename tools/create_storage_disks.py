#!/usr/bin/env python3
"""Builds the physical-controller disk images of the NVME_TEST in a folder.

storage-controller.img is an MBR disk with four primary partitions, FAT16,
ext2, ext3 and ext4, and 1 MiB left free at the end for the kernel test hook's
raw transfers. BuildNamespaceDisks makes the three namespaces of the
NVME_NAMESPACES_TEST: a GPT disk in 4096-byte sectors, a FAT16 superfloppy and
an image for a namespace with 8 bytes of metadata per block. BuildRootFourK
makes storage-root-4k.img for the ROOT_DISK_TEST: a GPT disk in 4096-byte
sectors whose first partition is an ext4 root with the programs."""

import argparse
import os
import sys
import tempfile
import uuid
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
ROOT_4K_IMAGE = "storage-root-4k.img"
ROOT_4K_SECTOR_SIZE = 4096
ROOT_4K_START = (1 << 20) // ROOT_4K_SECTOR_SIZE
ROOT_4K_SECTORS = (127 << 20) // ROOT_4K_SECTOR_SIZE
ROOT_4K_DISK_SECTORS = ROOT_4K_START + ROOT_4K_SECTORS + 256
ROOT_4K_UUID = "6f2d7e52-3c8e-4d4b-9a51-7a0e5b1c2d0b"
ROOT_4K_GUID = uuid.UUID("6F2D7E52-3C8E-4D4B-9A51-7A0E5B1C2D0C")
BLOCK_FILE = Path(__file__).resolve().parent.parent / "initramfs" / "block.txt"
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


def BuildRootFourK(directory, programs):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / ROOT_4K_IMAGE
    with open(path, "wb") as image:
        image.truncate(ROOT_4K_DISK_SECTORS * ROOT_4K_SECTOR_SIZE)
        disks.write_gpt(
            image,
            (
                (
                    disks.LINUX_FILESYSTEM_TYPE,
                    ROOT_4K_GUID,
                    ROOT_4K_START,
                    ROOT_4K_START + ROOT_4K_SECTORS - 1,
                    "FEATHEROS",
                ),
            ),
            ROOT_4K_SECTOR_SIZE,
            ROOT_4K_DISK_SECTORS,
        )
    environment = dict(os.environ, E2FSPROGS_FAKE_TIME=str(disks.EXT2_TIME))
    mke2fs = disks.find_mke2fs()
    with tempfile.TemporaryDirectory() as scratch:
        source = Path(scratch) / "tree"
        source.mkdir()
        names = disks.make_root_tree(source, BLOCK_FILE, programs)
        volume = Path(scratch) / "root.img"
        # 1024 four-KiB journal blocks: a shorter journal mounts read-only.
        disks.run_mke2fs(
            mke2fs,
            ("-t", "ext4", "-b", "4096", "-I", "256", "-J", "size=4"),
            "FEATHEROS",
            ROOT_4K_UUID,
            source,
            volume,
            ROOT_4K_SECTORS * ROOT_4K_SECTOR_SIZE // SECTOR_SIZE,
            environment,
            # A bare size counts blocks, not KiB, once -b is not 1024.
            "k",
        )
        disks.set_owners(
            disks.find_debugfs(mke2fs),
            volume,
            names,
            {f"{disks.SAMPLES}/{name}" for name in disks.EXT4_LINKS},
            lambda name: (0, 0),
            environment,
        )
        usb_disks.Copy(volume, path, ROOT_4K_START * ROOT_4K_SECTOR_SIZE)
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", help="where the image goes")
    parser.add_argument(
        "--programs",
        type=Path,
        help="the build's disk-programs folder, for storage-root-4k.img",
    )
    args = parser.parse_args()
    print(BuildControllerDisk(args.directory))
    for path in BuildNamespaceDisks(args.directory).values():
        print(path)
    if args.programs is not None:
        print(BuildRootFourK(args.directory, args.programs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
