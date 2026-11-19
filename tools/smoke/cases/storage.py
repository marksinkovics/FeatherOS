import os
import re
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

from create_test_disk import (
    E2FSPROGS_DIRECTORIES,
    EXT3_FILES,
    EXT3_START,
    EXT4_END,
    EXT4_START,
    FAT16_START,
    ROOT_START,
    SECTOR_SIZE,
    ext4_block,
)

from smoke.boot import PANIC_MARKER, ROOT_MESSAGE
from smoke.cases.base import SHELL_TIMEOUT_SECONDS, Case
from smoke.script import Lines

STORAGE_MESSAGE = (
    "KERN/STORAGE: sector, FAT16, ext2, ext3 and ext4 file writes verified; "
    "boot count "
)
# The journalled partitions: the kernel test (WriteVolumeFirstBoot and
# ChangeVolumeSecondBoot in kernel_test.cpp) writes them as it does the root.
JOURNALLED_PARTITIONS = {
    "ext3": (EXT3_START, EXT4_START),
    "ext4": (EXT4_START, EXT4_END),
}
# The superblock sits 1024 bytes into a volume: s_state at 0x3A (1 is clean)
# and the incompatible features at 0x60, where bit 0x4 is needs_recovery.
STATE_OFFSET = 1024 + 0x3A
INCOMPATIBLE_OFFSET = 1024 + 0x60
NEEDS_RECOVERY = 0x4
REPLAYED = re.compile(
    r"ext2: journal recovered; transactions replayed: 0x([0-9a-fA-F]+)"
)
# What the STORAGE kernel test (CheckExt2 in kernel_test.cpp) leaves on the
# ext2 root after each boot.
EXT2_DATA = b"".join(
    b"feather ext2 line %05d\n" % line for line in range(1000)
)
EXT2_FILES = {
    1: {
        "/os/data.txt": EXT2_DATA,
        "/os/data-link.txt": EXT2_DATA,
        "/os/sub/moved.txt": b"moved\n",
        "/os/short.txt": b"short\n",
    },
    2: {
        "/os/data-link.txt": EXT2_DATA,
        "/moved-sub/moved.txt": b"moved\n",
        "/os/short.txt": b"short\nmore\n",
    },
}
EXT2_STATS = {
    1: {
        "/os/data-link.txt": "Links: 2",
        "/os/to-data": 'Fast link dest: "data.txt"',
    },
    2: {"/os/data-link.txt": "Links: 1"},
}
EXT2_LISTINGS = {
    1: ("data.txt", "data-link.txt", "to-data", "sub", "short.txt"),
    2: ("data-link.txt", "short.txt"),
}
EXT2_GONE = {
    1: ("gone.txt", "empty", "scratch.txt"),
    2: ("data.txt", "to-data", "sub", "gone.txt"),
}
PERSISTENT_FILE_LINES = 200
# Shell commands for each STORAGE_TEST boot, in /home on the FAT16 partition,
# which nothing mounts at boot: the first writes notes.txt, and the second must
# still find it after the restart. sync is FAT16's device flush, since it has
# no periodic sync. The first boot then writes a file on the root and on the
# ext3 and ext4 partitions, with no sync or umount, and sleeps past the
# writeback task's 5 s: the harness stops QEMU after that, so only the
# periodic sync can have made the root clean and put the two journalled
# changes in their logs.
FAT16_SHELL_SETUP = b"mount -t fat16 vda2 /fat\ncd /fat/home\n"
STORAGE_SHELL_INPUT = {
    1: FAT16_SHELL_SETUP + b"write notes.txt first line\n"
    b"write -a notes.txt second line\n"
    b"cat notes.txt\nmkdir docs\nwrite docs/plan.txt nested hello\n"
    b'cat "/fat/read me first.txt"\nwrite "My Plans.txt" long name test\n'
    b"sync\nmount -t ext3 vda3 /ext3\nmount -t ext4 vda4 /ext4\n"
    b"write /periodic.txt periodic ext2\n"
    b"write /ext3/periodic.txt periodic ext3\n"
    b"write /ext4/periodic.txt periodic ext4\nsleep 8\nls docs\nls\n",
    2: FAT16_SHELL_SETUP + b"cat notes.txt\nrm notes.txt\ncat notes.txt\n"
    b"rm notes.txt\n"
    b"cat docs/plan.txt\nrmdir docs\nrm docs/plan.txt\nrmdir docs\n"
    b'cat "my plans.txt"\nrm "MY PLANS.TXT"\nsync\nls docs\nls\n',
}
NOTES_CONTENTS = b"first line\nsecond line\n"
PLAN_CONTENTS = b"nested hello\n"
LONG_NAME_CONTENTS = b"long name test\n"
NOTES_OUTPUT = re.compile(r"^first line\r?\nsecond line\r?$", re.MULTILINE)
# The FAT16 partition's root also holds the kernel test's PERSIST.TXT and
# Read Me First.txt, written by mtools.
STORAGE_SHELL_OUTPUT = {
    1: (
        "\nLong names from mtools work.",
        "plan.txt",
        "notes.txt",
        "My Plans.txt",
        "docs/",
        "1 directory, 2 files, ",
    ),
    2: (
        "cat: cannot open notes.txt",
        "rm: notes.txt: no such file",
        "\nnested hello",
        "rmdir: docs: Directory not empty",
        "\nlong name test",
        "ls: docs: no such directory",
        "0 files, ",
    ),
}


def VerifyDiskImage(disk_path, boot_count):
    """Checks the guest's FAT16 writes with mtools, a separate FAT reader."""
    partition_offset = FAT16_START * SECTOR_SIZE
    with open(disk_path, "rb") as disk:
        disk.seek(partition_offset)
        boot_sector = disk.read(512)
        reserved_sectors = struct.unpack_from("<H", boot_sector, 14)[0]
        fat_count = boot_sector[16]
        fat_bytes = struct.unpack_from("<H", boot_sector, 22)[0] * 512
        disk.seek(partition_offset + reserved_sectors * 512)
        fats = [disk.read(fat_bytes) for _ in range(fat_count)]
    if any(fat != fats[0] for fat in fats):
        return "the FAT copies differ"

    image = f"{disk_path}@@{partition_offset}"
    environment = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    expected = "".join(
        f"FeatherOS boot {boot_count} line {line}\n"
        for line in range(PERSISTENT_FILE_LINES)
    ).encode()
    contents = subprocess.run(
        ["mtype", "-i", image, "::PERSIST.TXT"],
        capture_output=True,
        env=environment,
        check=False,
    )
    if contents.returncode != 0 or contents.stdout != expected:
        return "mtools read different PERSIST.TXT contents"
    listing = subprocess.run(
        ["mdir", "-b", "-i", image, "::"],
        capture_output=True,
        text=True,
        env=environment,
        check=False,
    )
    if listing.returncode != 0 or "SCRATCH" in listing.stdout:
        return "mtools still lists the removed scratch file"
    notes = subprocess.run(
        ["mtype", "-i", image, "::home/notes.txt"],
        capture_output=True,
        env=environment,
        check=False,
    )
    plan = subprocess.run(
        ["mtype", "-i", image, "::home/docs/plan.txt"],
        capture_output=True,
        env=environment,
        check=False,
    )
    if boot_count == 1 and notes.stdout != NOTES_CONTENTS:
        return "mtools read different home/notes.txt contents"
    long_name = subprocess.run(
        ["mtype", "-i", image, "::home/My Plans.txt"],
        capture_output=True,
        env=environment,
        check=False,
    )
    if boot_count == 1 and plan.stdout != PLAN_CONTENTS:
        return "mtools read different home/docs/plan.txt contents"
    if boot_count == 1 and long_name.stdout != LONG_NAME_CONTENTS:
        return "mtools read different 'My Plans.txt' contents"
    home_listing = subprocess.run(
        ["mdir", "-b", "-i", image, "::home"],
        capture_output=True,
        text=True,
        env=environment,
        check=False,
    )
    listed = home_listing.stdout.upper()
    if boot_count == 2 and any(
        name in listed for name in ("NOTES", "DOCS", "MY PLANS")
    ):
        return "mtools still lists a file or directory the shell removed"
    return None


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


def VerifyExtVolume(volume, boot_count, name="ext2"):
    """debugfs, a reader independent of the kernel, must see what the guest
    wrote on the volume (name is its type), once e2fsck -fn finds nothing to
    fix."""
    checked = E2fsck(volume)
    if checked is not None:
        return checked
    debugfs = FindE2fsprogs("debugfs")
    if debugfs is None:
        return "debugfs not found; install e2fsprogs"
    files = dict(EXT2_FILES[boot_count])
    expected_names = list(EXT2_LISTINGS[boot_count])
    text = f"periodic {name}\n".encode()
    if boot_count == 1:
        files["/periodic.txt"] = text
    elif name != "ext2":
        files["/os/replayed.txt"] = text
        expected_names.append("replayed.txt")
    for path, contents in files.items():
        if Debugfs(debugfs, volume, f"cat {path}") != contents:
            return f"debugfs read different {path} contents"
    for path, stat_text in EXT2_STATS[boot_count].items():
        if stat_text not in Debugfs(debugfs, volume, f"stat {path}").decode():
            return f"debugfs stat {path} does not show {stat_text!r}"
    listing = Debugfs(debugfs, volume, "ls -p /os").decode().splitlines()
    names = {line.split("/")[5] for line in listing if line.count("/") >= 6}
    missing = [name for name in expected_names if name not in names]
    if missing:
        return f"debugfs does not list {missing} in /os"
    left = [name for name in EXT2_GONE[boot_count] if name in names]
    if left:
        return f"debugfs still lists {left} in /os"
    if boot_count == 2 and not NotFound(debugfs, volume, "/periodic.txt"):
        return "debugfs still finds /periodic.txt"
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


def VerifyExt2(disk_path, boot_count=None):
    """Checks the root's ext2 partition, copied out of the disk image: with
    e2fsck alone without a boot count, else also what that boot of the STORAGE
    test wrote, and that the volume is marked clean, which the writeback task
    does within 5 s of the shell session's last change."""
    with tempfile.TemporaryDirectory() as work:
        volume = Path(work) / "root.img"
        with open(disk_path, "rb") as disk, volume.open("wb") as copy:
            disk.seek(ROOT_START * SECTOR_SIZE)
            copy.write(disk.read((FAT16_START - ROOT_START) * SECTOR_SIZE))
        if boot_count is None:
            return E2fsck(str(volume))
        if not IsMarkedClean(volume):
            return "the root is not marked clean"
        return VerifyExtVolume(str(volume), boot_count)


def VerifyJournalled(disk_path, boot_count):
    """Checks the ext3 and ext4 partitions on copies. After the first boot the
    shell session stopped them mounted: the change the writeback task
    committed is only in the log, so e2fsck -fy replays it on the copy. After
    the second, the kernel test replayed it and unmounted, so they are clean
    as they are."""
    debugfs = FindE2fsprogs("debugfs")
    e2fsck = FindE2fsprogs("e2fsck")
    if debugfs is None or e2fsck is None:
        return "e2fsprogs not found; install it"
    for name, (first, end) in JOURNALLED_PARTITIONS.items():
        with tempfile.TemporaryDirectory() as work:
            volume = Path(work) / f"{name}.img"
            volume.write_bytes(PartitionBytes(disk_path, first, end))
            if boot_count == 1:
                if not NeedsRecovery(volume):
                    return f"{name} was stopped mounted but needs no recovery"
                if not NotFound(debugfs, str(volume), "/periodic.txt"):
                    return (
                        f"{name}: /periodic.txt is on disk before the replay"
                    )
                repaired = subprocess.run(
                    [e2fsck, "-fy", str(volume)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                if repaired.returncode not in (0, 1):
                    return (
                        f"{name}: e2fsck -fy exited with "
                        f"{repaired.returncode}:\n{repaired.stdout}"
                    )
            elif NeedsRecovery(volume):
                return f"{name}: needs recovery after the second boot"
            failure = VerifyExtVolume(str(volume), boot_count, name)
        if failure is not None:
            return f"{name}: {failure}"
    return None


def VerifyJournalledClean(disk_path):
    """After the shell session, which unmounts both: nothing to recover,
    e2fsck -fn finds nothing, and the session's file is there and what it
    removed is not."""
    debugfs = FindE2fsprogs("debugfs")
    if debugfs is None:
        return "debugfs not found; install e2fsprogs"
    extents = b"".join(ext4_block(block) for block in range(2048))
    for name, (first, end) in JOURNALLED_PARTITIONS.items():
        with tempfile.TemporaryDirectory() as work:
            volume = Path(work) / f"{name}.img"
            volume.write_bytes(PartitionBytes(disk_path, first, end))
            if NeedsRecovery(volume):
                return f"{name} needs recovery after the unmount"
            checked = E2fsck(str(volume))
            if checked is not None:
                return f"{name}: {checked}"
            if (
                Debugfs(debugfs, str(volume), "cat /shell.txt")
                != f"shell wrote {name}\n".encode()
            ):
                return f"{name}: debugfs read a different /shell.txt"
            for gone in ("/made", "/copy.bin"):
                if not NotFound(debugfs, str(volume), gone):
                    return f"{name}: {gone} is still there"
            original = (
                ("/extents.bin", extents)
                if name == "ext4"
                else ("/big.txt", EXT3_FILES["big.txt"])
            )
            if (
                Debugfs(debugfs, str(volume), f"cat {original[0]}")
                != original[1]
            ):
                return f"{name}: {original[0]} changed"
    return None


def CheckSecondBoot(output):
    """The root was left clean without a final sync, and each journalled
    partition's journal was replayed by a writable mount."""
    if ROOT_MESSAGE not in output:
        return f"the root did not mount read-write: expected {ROOT_MESSAGE!r}"
    replayed = [int(count, 16) for count in REPLAYED.findall(output)]
    if len(replayed) < 2 or min(replayed) < 1:
        return f"expected two journal replays of 1+ transactions: {replayed}"
    return None


class Storage(Case):
    timeout = SHELL_TIMEOUT_SECONDS
    serial_input = True

    def __init__(self, args):
        super().__init__(args)
        self.boot = args.expect_storage_boot

    def Script(self, session):
        return Lines(STORAGE_SHELL_INPUT[self.boot])

    def Markers(self):
        return (
            STORAGE_SHELL_OUTPUT[self.boot][-1].encode(),
            PANIC_MARKER.encode(),
        )

    def Check(self, result):
        output = result.output
        expected = f"{STORAGE_MESSAGE}{self.boot}"
        if (
            re.search(rf"^{re.escape(expected)}\r?$", output, re.MULTILINE)
            is None
        ):
            return f"expected '{expected}'"
        missing = [
            text
            for text in STORAGE_SHELL_OUTPUT[self.boot]
            if text not in output
        ]
        if missing or NOTES_OUTPUT.search(output) is None:
            return (
                "file tools output is missing "
                f"{missing or 'the cat output'}{result.limit_note}"
            )
        disk_error = VerifyDiskImage(self.args.disk, self.boot)
        if disk_error is not None:
            return f"disk image check: {disk_error}"
        ext2_error = VerifyExt2(self.args.disk, self.boot)
        if ext2_error is not None:
            return f"ext2 root check: {ext2_error}"
        journalled_error = VerifyJournalled(self.args.disk, self.boot)
        if journalled_error is not None:
            return f"ext3/ext4 check: {journalled_error}"
        if self.boot == 2:
            return CheckSecondBoot(output)
        return None

    def PassMessage(self, result):
        return (
            "sector, FAT16, ext2 and shell file-tool writes verified in the "
            "guest, with mtools, e2fsck and debugfs, and the writeback task "
            f"synced ext2, ext3 and ext4; boot {self.boot} persisted"
        )
