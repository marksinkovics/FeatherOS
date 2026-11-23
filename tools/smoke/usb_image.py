"""The USB stick image of the USB_STORAGE test: an MBR, a FAT16 partition and
an ext2 partition, built with mtools and e2fsprogs through create_test_disk."""

import subprocess
import tempfile
from pathlib import Path

import create_test_disk as disks

PARTITION_SECTORS = 8192
FAT16_START = 2048
EXT2_START = FAT16_START + PARTITION_SECTORS
IMAGE_SECTORS = EXT2_START + PARTITION_SECTORS
FAT16_FILE = "USBFILE.TXT"
FAT16_CONTENTS = b"fat16 on a stick\n"


def BuildImage(path):
    with open(path, "wb") as image:
        image.truncate(IMAGE_SECTORS * disks.SECTOR_SIZE)
        disks.write_mbr(
            image,
            disks.mbr_entry(
                disks.FAT16_PARTITION_TYPE, FAT16_START, PARTITION_SECTORS
            ),
            disks.mbr_entry(
                disks.LINUX_PARTITION_TYPE, EXT2_START, PARTITION_SECTORS
            ),
        )
    spec = f"{path}@@{FAT16_START * disks.SECTOR_SIZE}"
    subprocess.run(
        [
            "mformat",
            "-i",
            spec,
            "-H",
            str(FAT16_START),
            "-T",
            str(PARTITION_SECTORS),
            "-c",
            "1",
            "-v",
            "USBFAT",
            "::",
        ],
        check=True,
    )
    with tempfile.TemporaryDirectory() as scratch:
        source = Path(scratch) / FAT16_FILE
        source.write_bytes(FAT16_CONTENTS)
        subprocess.run(
            ["mcopy", "-i", spec, str(source), f"::{FAT16_FILE}"], check=True
        )
        volume = Path(scratch) / "ext2.img"
        disks.make_ext2_volume(volume, PARTITION_SECTORS)
        disks.copy_volume(volume, Path(path), EXT2_START)
