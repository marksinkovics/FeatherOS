import re
import shutil
import tempfile
from pathlib import Path

import create_storage_disks as storage_disks
import create_usb_disks as usb_disks
from create_storage_disks import SECTOR_SIZE

from smoke.boot import PANIC_MARKER, ROOT_MESSAGE
from smoke.cases.base import SHELL_TIMEOUT_SECONDS, Case
from smoke.disk_checks import (
    Debugfs,
    E2fsck,
    FatCopiesAgree,
    FatStatusWord,
    FindE2fsprogs,
    FindFsckFat,
    FsckFat,
    IsMarkedClean,
    Mdir,
    MtoolsSpec,
    Mtype,
    NeedsRecovery,
    NotFound,
    PartitionBytes,
)
from smoke.script import Lines, WaitFor

DISK_KIBIBYTES = storage_disks.CONTROLLER_SECTORS * SECTOR_SIZE // 1024


def Present(output, line):
    if isinstance(line, str):
        return line in output
    return line.search(output) is not None


class Naming:
    """How one controller kind names its disk, volumes and hook lines."""

    def __init__(self, disk, separator, tag, kind, mounted):
        self.disk = disk
        self.tag = tag
        self.kind = kind
        # Name, filesystem, first sector, sector count.
        self.volumes = tuple(
            (f"{disk}{separator}{number}", filesystem, start, sectors)
            for number, (filesystem, start, sectors) in enumerate(
                (
                    (
                        "fat16",
                        storage_disks.FAT16_START,
                        storage_disks.FAT16_SECTORS,
                    ),
                    (
                        "ext2",
                        storage_disks.EXT2_START,
                        storage_disks.EXT2_SECTORS,
                    ),
                    (
                        "ext3",
                        storage_disks.EXT3_START,
                        storage_disks.EXT3_SECTORS,
                    ),
                    (
                        "ext4",
                        storage_disks.EXT4_START,
                        storage_disks.EXT4_SECTORS,
                    ),
                ),
                start=1,
            )
        )
        self.sectors_line = (
            f"KERN/{tag}: {disk} {storage_disks.CONTROLLER_SECTORS} sectors "
            f"of {SECTOR_SIZE} bytes"
        )
        self.steps = (
            f"KERN/{tag}: {mounted}",
            f"KERN/{tag}: last sector written and read back",
            f"KERN/{tag}: transfer past the end refused",
            f"KERN/{tag}: multi-command transfer matched",
            f"KERN/{tag}: counters matched",
            f"KERN/{tag}: tests passed",
        )


NVME = Naming(
    "nvme0n1",
    "p",
    "NVME",
    "nvme",
    "nvme0n1p1 to p4 mounted, read and unmounted",
)
VOLUMES = NVME.volumes
CONTROLLER_LINE = re.compile(
    r"^KERN/I: Storage: nvme0: [0-9a-f]{2}:[0-9a-f]{2}\.0 \"QEMU NVMe Ctrl\" "
    r"fw [^,]+, (MSI-X|MSI|polling), 1 I/O queue of 32, max transfer "
    r"\d+ KiB, volatile write cache\r?$",
    re.MULTILINE,
)
POLICY_LINE = "KERN/I: Storage: nvme0: QEMU controller, writes allowed"
DISK_LINE = (
    f"KERN/I: Storage: nvme0n1: {DISK_KIBIBYTES} KiB, {SECTOR_SIZE}-byte "
    "sectors, rw"
)
FAILURES = (
    "command timed out",
    "controller failed",
    "stale completion",
    "could not",
    "did not complete",
)
AHCI_FAILURES = FAILURES + (
    "failed (status",
    "no recovery",
    "port failed",
    "did not stop",
    "not ready",
)
LAST = b"written just before shutdown\n"
HELLO = {
    "ext2": (b"/hello.txt", b"Hello from the ext2 partition.\n"),
    "ext3": (b"/hello.txt", b"Hello from the ext3 partition.\n"),
    "ext4": (b"/hello.txt", b"Hello from the second ext4 partition.\n"),
}


def Small(number):
    return f"nvme small file p{number}\n".encode()


def Inner(number):
    return f"inner file p{number}\n".encode()


def VolumeScript(naming, number, kind):
    point = f"/tmp/n{number}"
    source = naming.volumes[number - 1][0]
    big = (
        f"cp {point}/BIG.BIN {point}/COPY.BIN"
        if number == 1
        else f"cp /tmp/n1/BIG.BIN {point}/COPY.BIN"
    )
    lines = [
        f"mkdir {point}",
        f"mount -t {kind} {source} {point}",
        f"grep {source} /proc/mounts",
        f"write {point}/small.txt nvme small file p{number}",
        f"cat {point}/small.txt",
        big,
        f"echo big copy p{number} done",
        f"mkdir {point}/dir",
        f"write {point}/dir/inner.txt inner file p{number}",
        f"mv {point}/dir/inner.txt {point}/dir/moved.txt",
        f"write {point}/gone.txt temporary",
        f"rm {point}/gone.txt",
        f"ls {point}/dir",
        "sync",
        f"umount {point}",
        f"mount -t {kind} {source} {point}",
        f"cat {point}/small.txt",
        f"cat {point}/dir/moved.txt",
    ]
    if number != 1:
        lines.append(f"umount {point}")
    return lines


def ShellInput(naming):
    lines = ["sleep 1", "cat /proc/partitions"]
    for number, (_, kind, _, _) in enumerate(naming.volumes, start=1):
        lines += VolumeScript(naming, number, kind)
    lines += [
        "umount /tmp/n1",
        f"mount -t ext4 {naming.volumes[3][0]} /tmp/n4",
        "write /tmp/n4/last.txt written just before shutdown",
        "shutdown",
    ]
    return ("\n".join(lines) + "\n").encode()


def ShellOutput(naming):
    expected = []
    for number, (source, kind, _, _) in enumerate(naming.volumes, start=1):
        expected += [
            f"{source} /tmp/n{number} {kind} rw 0 0",
            f"nvme small file p{number}",
            f"big copy p{number} done",
            re.compile(r"moved\.txt +\d+"),
            f"inner file p{number}",
        ]
    return tuple(expected)


KERNEL_MESSAGES = (
    "KERN/I: Storage: nvme0: shutdown complete",
    "KERN/I: Shutdown: stopping processes",
    "KERN/I: Shutdown: disk flushed",
    "KERN/I: Powering off",
)


class Nvme(Case):
    """An emulated NVMe controller with one namespace over a scratch copy of
    storage-controller.img from create_storage_disks.py: an MBR with FAT16,
    ext2, ext3 and ext4 partitions and a free tail. The kernel probes the
    controller on the storage worker, registers nvme0n1 and opens the four
    volumes. The test hook mounts each read-only and reads a file, then writes
    and reads a pattern in the free tail, checks that transfers past the end
    are refused and that 512 KiB takes several commands, all through the
    registry's disk, whose counters it checks. The shell then mounts each
    volume by name, writes a file, copies 1 MiB, makes a directory, renames
    and removes files, syncs, unmounts, mounts again and reads, and shuts
    down with the ext4 volume mounted right after a write. The harness checks
    the image with mtools, e2fsck and, when installed, fsck.fat, and that the
    root stayed on vda1."""

    timeout = SHELL_TIMEOUT_SECONDS
    serial_input = True
    shuts_down = True
    passed = (
        "the NVMe namespace was probed, four volumes were written and read "
        "back, and the image passed the host checks"
    )

    naming = NVME
    controller_line = CONTROLLER_LINE
    policy_line = POLICY_LINE
    disk_line = DISK_LINE
    kernel_messages = KERNEL_MESSAGES
    failures = FAILURES
    what = "NVMe"

    def __init__(self, args):
        super().__init__(args)
        self.directory = Path(args.disk).parent / "storage-disks"
        self.image = None
        self.fsck_note = ""
        self.interrupts = "unknown"

    def Start(self):
        shutil.rmtree(self.directory, ignore_errors=True)
        self.image = storage_disks.BuildControllerDisk(self.directory)

    def QemuArguments(self):
        return [
            "-drive",
            f"if=none,id=nvme_disk,file={self.image},format=raw",
            "-device",
            "nvme,serial=feather-nvme0,drive=nvme_disk",
        ]

    def Script(self, session):
        return [
            WaitFor(self.naming.steps[-1]),
            *Lines(ShellInput(self.naming)),
        ]

    def Markers(self):
        return (PANIC_MARKER.encode(),)

    def Check(self, result):
        output = result.output
        if PANIC_MARKER in output:
            return f"the kernel panicked{result.limit_note}"
        if "KERN/W: Shutdown:" in output:
            return "the shutdown reported a problem"
        return (
            self.CheckBoot(output)
            or self.CheckHook(output)
            or self.CheckShell(output, result)
            or self.CheckImage()
        )

    def CheckBoot(self, output):
        found = self.controller_line.search(output)
        if found is None:
            return f"the boot log lacks the {self.what} controller line"
        self.interrupts = found.group(1)
        if self.interrupts == "polling":
            return "the controller fell back to polling"
        for line in (self.policy_line, self.disk_line, ROOT_MESSAGE):
            if not Present(output, line):
                return f"the log lacks '{getattr(line, 'pattern', line)}'"
        for source, kind, _, sectors in self.naming.volumes:
            kibibytes = sectors * SECTOR_SIZE // 1024
            pattern = re.compile(
                rf"KERN/I: Storage: {source}: {kind}, {kibibytes} KiB\r?$",
                re.MULTILINE,
            )
            if pattern.search(output) is None:
                return f"the worker did not log the volume {source}"
        return None

    def CheckHook(self, output):
        if self.naming.sectors_line not in output:
            return f"the test hook did not report '{self.naming.sectors_line}'"
        position = 0
        for step in self.naming.steps:
            found = output.find(step, position)
            if found < 0:
                return f"the test hook never reported '{step}' in order"
            position = found
        for line in output.splitlines():
            if "KERN/" in line and any(text in line for text in self.failures):
                return f"the log reports a {self.what} failure: {line.strip()}"
        return None

    def CheckShell(self, output, result):
        naming = self.naming
        pattern = re.compile(
            rf"^{naming.disk} +{DISK_KIBIBYTES} {SECTOR_SIZE} "
            rf"{naming.kind}\r?$",
            re.MULTILINE,
        )
        if pattern.search(output) is None:
            return f"/proc/partitions lacks the disk line of {naming.disk}"
        for source, kind, _, sectors in naming.volumes:
            kibibytes = sectors * SECTOR_SIZE // 1024
            pattern = re.compile(
                rf"^{source} +{kibibytes} {SECTOR_SIZE} {kind}\r?$",
                re.MULTILINE,
            )
            if pattern.search(output) is None:
                return f"/proc/partitions lacks the volume line of {source}"
        for line in ShellOutput(naming):
            text = line if isinstance(line, str) else line.pattern
            pattern = re.escape(line) if isinstance(line, str) else text
            if re.search(rf"^{pattern}\r?$", output, re.MULTILINE) is None:
                return (
                    f"the shell session never printed '{text}'"
                    f"{result.limit_note}"
                )
        for message in self.kernel_messages:
            if message not in output:
                return f"the log lacks '{message}'{result.limit_note}"
        if not result.exited_by_itself:
            return "QEMU kept running after shutdown"
        return None

    def CheckImage(self):
        failure = self.CheckFat16() or self.CheckExt()
        if FindFsckFat() is None:
            self.fsck_note = "; fsck.fat is not installed, skipped"
        return failure

    def CheckFat16(self):
        label = self.naming.volumes[0][0]
        start = storage_disks.FAT16_START
        if not FatCopiesAgree(self.image, start):
            return f"{label}: the FAT copies differ"
        spec = MtoolsSpec(self.image, start)
        big = usb_disks.BigContents()
        expected = {
            storage_disks.FAT16_FILE: storage_disks.FAT16_CONTENTS,
            storage_disks.BIG_FILE: big,
            "COPY.BIN": big,
            "small.txt": Small(1),
            "dir/moved.txt": Inner(1),
        }
        for name, contents in expected.items():
            if Mtype(spec, name) != contents:
                return f"{label}: mtools read different {name} contents"
        if "GONE" in (Mdir(spec) or ""):
            return f"{label}: mtools still lists gone.txt"
        if "INNER" in (Mdir(spec, "dir") or ""):
            return f"{label}: mtools still lists dir/inner.txt"
        if FatStatusWord(self.image, start) != 0xFFFF:
            return f"{label}: FAT[1] changed"
        checked = FsckFat(
            self.image, start, start + storage_disks.FAT16_SECTORS
        )
        if checked is not None:
            return f"{label}: {checked}"
        return None

    def CheckExt(self):
        debugfs = FindE2fsprogs("debugfs")
        if debugfs is None:
            return "debugfs not found; install e2fsprogs"
        big = usb_disks.BigContents()
        with tempfile.TemporaryDirectory() as work:
            for number, (source, kind, start, sectors) in enumerate(
                self.naming.volumes[1:], start=2
            ):
                volume = Path(work) / f"{source}.img"
                volume.write_bytes(
                    PartitionBytes(self.image, start, start + sectors)
                )
                failure = self.CheckVolume(
                    debugfs, volume, source, kind, number, big
                )
                if failure is not None:
                    return failure
        return None

    def CheckVolume(self, debugfs, volume, source, kind, number, big):
        if not IsMarkedClean(volume):
            return f"{source} is not marked clean after the umount"
        if NeedsRecovery(volume):
            return f"{source} needs journal recovery"
        checked = E2fsck(str(volume))
        if checked is not None:
            return f"{source}: {checked}"
        name, contents = HELLO[kind]
        expected = {
            b"/small.txt": Small(number),
            b"/dir/moved.txt": Inner(number),
            b"/COPY.BIN": big,
            name: contents,
        }
        if kind == "ext4":
            expected[b"/last.txt"] = LAST
        for path, data in expected.items():
            if Debugfs(debugfs, str(volume), f"cat {path.decode()}") != data:
                return f"{source}: debugfs read a different {path.decode()}"
        for path in ("/gone.txt", "/dir/inner.txt"):
            if not NotFound(debugfs, str(volume), path):
                return f"{source}: {path} is still there"
        return None

    def PassMessage(self, result):
        return f"{self.passed} (interrupts: {self.interrupts}){self.fsck_note}"
