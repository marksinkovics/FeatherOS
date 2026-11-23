"""Host-side checks of a disk image after a QEMU run.

mtools for FAT16, e2fsprogs for ext2 to ext4, and fsck.fat when it is
installed. Shared by the storage, power, root and USB storage cases."""

import os
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

from create_test_disk import E2FSPROGS_DIRECTORIES, SECTOR_SIZE

# The superblock sits 1024 bytes into a volume: s_state at 0x3A (1 is clean)
# and the incompatible features at 0x60, where bit 0x4 is needs_recovery.
STATE_OFFSET = 1024 + 0x3A
INCOMPATIBLE_OFFSET = 1024 + 0x60
NEEDS_RECOVERY = 0x4


def FindE2fsprogs(name):
    """Homebrew's keg-only e2fsprogs first, as create_test_disk.py looks."""
    for directory in E2FSPROGS_DIRECTORIES:
        candidate = Path(directory) / name
        if candidate.is_file():
            return str(candidate)
    return shutil.which(name) or shutil.which(name, path="/usr/sbin:/sbin")


def Debugfs(debugfs, volume, command):
    return subprocess.run(
        [debugfs, "-R", command, volume], capture_output=True, check=False
    ).stdout


def RunDebugfsWrite(debugfs, volume, commands_file):
    """Runs debugfs -w over a command file, failing on any error."""
    done = subprocess.run(
        [debugfs, "-w", "-f", commands_file, volume],
        capture_output=True,
        text=True,
        check=True,
    )
    return done.stdout


def DebugfsMessages(debugfs, volume, command):
    """What debugfs prints for the command, errors included."""
    done = subprocess.run(
        [debugfs, "-R", command, volume], capture_output=True, check=False
    )
    return (done.stdout + done.stderr).decode(errors="replace")


def E2fsck(volume):
    """e2fsck -fn must find nothing to fix."""
    e2fsck = FindE2fsprogs("e2fsck")
    if e2fsck is None:
        return "e2fsck not found; install e2fsprogs"
    check = subprocess.run(
        [e2fsck, "-fn", volume], capture_output=True, text=True, check=False
    )
    if check.returncode != 0:
        return (
            f"e2fsck -fn exited with {check.returncode}:\n"
            f"{check.stdout}{check.stderr}"
        )
    return None


def NotFound(debugfs, volume, path):
    return "File not found" in DebugfsMessages(debugfs, volume, f"stat {path}")


def PartitionBytes(disk_path, first, end):
    with open(disk_path, "rb") as disk:
        disk.seek(first * SECTOR_SIZE)
        return disk.read((end - first) * SECTOR_SIZE)


def NeedsRecovery(volume):
    with open(volume, "rb") as image:
        image.seek(INCOMPATIBLE_OFFSET)
        return bool(image.read(1)[0] & NEEDS_RECOVERY)


def IsMarkedClean(volume):
    with open(volume, "rb") as image:
        image.seek(STATE_OFFSET)
        return struct.unpack("<H", image.read(2))[0] & 1 == 1


def ReplayOnCopy(volume):
    """What a mount that takes writes does: e2fsck -fy replays the journal."""
    e2fsck = FindE2fsprogs("e2fsck")
    if e2fsck is None:
        return "e2fsck not found; install e2fsprogs"
    repaired = subprocess.run(
        [e2fsck, "-fy", str(volume)],
        capture_output=True,
        text=True,
        check=False,
    )
    if repaired.returncode not in (0, 1):
        return (
            f"e2fsck -fy exited with {repaired.returncode}:\n{repaired.stdout}"
        )
    return None


def MtoolsEnvironment():
    return dict(os.environ, MTOOLS_SKIP_CHECK="1")


def MtoolsSpec(image_path, first_sector=0):
    """The -i argument for the volume starting at first_sector of an image."""
    if first_sector == 0:
        return str(image_path)
    return f"{image_path}@@{first_sector * SECTOR_SIZE}"


def Mtype(spec, name):
    """The bytes of ::name on an image, or None when mtools cannot read it."""
    done = subprocess.run(
        ["mtype", "-i", spec, f"::{name}"],
        capture_output=True,
        env=MtoolsEnvironment(),
        check=False,
    )
    return done.stdout if done.returncode == 0 else None


def Mdir(spec, directory=""):
    """The upper-cased short and long names in ::directory, or None."""
    done = subprocess.run(
        ["mdir", "-b", "-i", spec, f"::{directory}"],
        capture_output=True,
        text=True,
        env=MtoolsEnvironment(),
        check=False,
    )
    if done.returncode != 0:
        return None
    return done.stdout.upper()


def FatCopiesAgree(image_path, first_sector=0):
    """Every FAT copy of the FAT16 volume holds the same bytes."""
    offset = first_sector * SECTOR_SIZE
    with open(image_path, "rb") as image:
        image.seek(offset)
        boot_sector = image.read(SECTOR_SIZE)
        reserved_sectors = struct.unpack_from("<H", boot_sector, 14)[0]
        fat_count = boot_sector[16]
        fat_bytes = struct.unpack_from("<H", boot_sector, 22)[0] * SECTOR_SIZE
        image.seek(offset + reserved_sectors * SECTOR_SIZE)
        fats = [image.read(fat_bytes) for _ in range(fat_count)]
    return all(fat == fats[0] for fat in fats)


def FatStatusWord(volume_path, first_sector=0):
    """FAT[1] of a FAT16 volume with one reserved sector, as the guest left
    it."""
    with open(volume_path, "rb") as image:
        image.seek((first_sector + 1) * SECTOR_SIZE + 2)
        return struct.unpack("<H", image.read(2))[0]


def FindFsckFat():
    return shutil.which("fsck.fat") or shutil.which(
        "fsck.fat", path="/usr/sbin:/sbin:/opt/homebrew/sbin"
    )


def FsckFat(image_path, first_sector, end_sector):
    """fsck.fat -n on a copy of the volume: nothing for it to fix. Returns
    None when it passes, and also when it is not installed (FindFsckFat tells
    the cases to say so)."""
    fsck = FindFsckFat()
    if fsck is None:
        return None
    with tempfile.TemporaryDirectory() as work:
        volume = Path(work) / "fat16.img"
        volume.write_bytes(
            PartitionBytes(image_path, first_sector, end_sector)
        )
        done = subprocess.run(
            [fsck, "-n", str(volume)],
            capture_output=True,
            text=True,
            check=False,
        )
    if done.returncode != 0:
        return f"fsck.fat -n exited with {done.returncode}:\n{done.stdout}"
    return None
