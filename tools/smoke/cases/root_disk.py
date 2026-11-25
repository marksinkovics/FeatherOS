import hashlib
import re
import shutil
import tempfile
from pathlib import Path

import create_storage_disks as storage_disks
from create_test_disk import (
    DATA_END,
    DATA_START,
    EXT2_END,
    EXT2_START,
    EXT3_END,
    EXT3_START,
    FAT16_START,
    ROOT_START,
    SECTOR_SIZE,
)

from smoke.boot import (
    INIT_MESSAGE,
    PANIC_MARKER,
    RESCUE_INIT_MESSAGE,
    SHELL_BANNER,
)
from smoke.cases.base import SHELL_TIMEOUT_SECONDS, Case
from smoke.disk_checks import (
    Debugfs,
    E2fsck,
    FindE2fsprogs,
    IsMarkedClean,
    NeedsRecovery,
    PartitionBytes,
)
from smoke.script import Lines

# The bus's disk name and the character between it and a partition number.
DISKS = {
    "virtio": ("vda", ""),
    "nvme": ("nvme0n1", "p"),
    "nvme4k": ("nvme0n1", "p"),
    "ahci": ("sda", ""),
}
# Partition number: type and range in 512-byte sectors of the test disk's MBR.
TEST_DISK = {
    1: ("ext4", ROOT_START, FAT16_START),
    2: ("fat16", FAT16_START, EXT3_START),
    3: ("ext3", EXT3_START, EXT3_END),
    5: ("ext4", DATA_START, DATA_END),
    6: ("ext2", EXT2_START, EXT2_END),
}
ROOT_FOUR_K = {
    1: (
        "ext4",
        storage_disks.ROOT_4K_START * 8,
        (storage_disks.ROOT_4K_START + storage_disks.ROOT_4K_SECTORS) * 8,
    )
}
ROOT_NAME = re.compile(r"^(vd[a-z]|sd[a-z]|nvme\d+n\d+p)(\d+)$")
WRITTEN = b"written on the root disk\n"
POLICY_REASON = "read-only: the storage policy (storage=rw to write)"
ROOT_ETC_RAM_COPY = (
    "KERN/I: VFS: /etc is the initramfs's RAM copy: the root is read-only"
)
ROOT_ETC_OWN = "KERN/I: VFS: /etc is the root's own directory"
TMPFS_ROOT = "KERN/I: VFS: / is tmpfs root, read-write"
REFUSED_WRITE = "write: cannot open /rootdisk.txt: Read-only file system"


def Digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class RootDisk(Case):
    """The test disk attached by DISK_BUS (virtio-blk, an NVMe namespace, an
    AHCI disk, or an NVMe namespace of 4096-byte blocks with its own root
    image) and booted with the kernel's root= as KERNEL_ROOT names it, with
    storage=KERNEL_STORAGE. The kernel must mount the root that the options
    and the bus decide (a named partition, the named disk's first volume with
    a warning, vda's default after a warning, or the RAM root), log it with
    its mode, and start init. On a writable root the shell writes a file; on
    a read-only one the write is refused and the image's SHA-256 stays the
    same; with the RAM root and a disk on NVMe the shell mounts the disk's
    volume itself. The harness then checks the root volume with e2fsck and
    debugfs."""

    timeout = SHELL_TIMEOUT_SECONDS
    serial_input = True
    shuts_down = True

    def __init__(self, args):
        super().__init__(args)
        self.bus = args.disk_bus
        self.disk, self.separator = DISKS[self.bus]
        self.volumes = ROOT_FOUR_K if self.bus == "nvme4k" else TEST_DISK
        self.sector_unit = 8 if self.bus == "nvme4k" else 1
        self.root = None
        self.warning = None
        self.number = self.Decide(args.kernel_root)
        # Only the NVMe and AHCI controllers have a write policy.
        self.read_only = args.kernel_storage == "ro" and self.bus != "virtio"
        self.directory = Path(args.disk).parent / "root-disk"
        self.image = None
        self.digest = None

    def Name(self, number):
        return f"{self.disk}{self.separator}{number}"

    def Decide(self, requested):
        """The partition number of the disk that should be the root, or None
        for the RAM root."""
        if requested:
            found = ROOT_NAME.match(requested)
            if found is None:
                raise ValueError(f"unexpected root= {requested}")
            disk = found.group(1).removesuffix("p")
            if disk == self.disk:
                number = int(found.group(2))
                if number in self.volumes:
                    return number
                self.warning = (
                    f"KERN/W: VFS: root={requested} is not a usable "
                    "partition, using the default root"
                )
                return min(self.volumes)
            self.warning = (
                f"KERN/W: VFS: root={requested}: no disk {disk}, "
                "using the default root"
            )
        return 1 if self.bus == "virtio" else None

    def Start(self):
        shutil.rmtree(self.directory, ignore_errors=True)
        if self.bus == "nvme4k":
            programs = Path(self.args.build_dir) / "disk-programs"
            self.image = str(
                storage_disks.BuildRootFourK(self.directory, programs)
            )
        else:
            self.image = self.args.disk
        self.digest = Digest(self.image)

    def DiskImage(self):
        return self.image if self.bus == "nvme4k" else None

    def Script(self, session):
        if self.number is None:
            return Lines(
                b"sleep 2\n"
                b"cat /proc/partitions\n"
                b"mkdir /mnt\n"
                + f"mount -t ext4 {self.Name(1)} /mnt\n".encode()
                + f"grep {self.Name(1)} /proc/mounts\n".encode()
                + b"ls /mnt\n"
                b"umount /mnt\n"
                b"shutdown\n"
            )
        return Lines(
            b"write /rootdisk.txt written on the root disk\n"
            b"cat /rootdisk.txt\n"
            b"cat /proc/mounts\n"
            b"cat /proc/partitions\n"
            b"shutdown\n"
        )

    def Markers(self):
        return (PANIC_MARKER.encode(),)

    def Check(self, result):
        output = result.output
        if PANIC_MARKER in output:
            return f"the kernel panicked{result.limit_note}"
        return (
            self.CheckBoot(output, result)
            or self.CheckShell(output, result)
            or self.CheckImage(result)
        )

    def RootType(self):
        return self.volumes[self.number][0]

    def CheckBoot(self, output, result):
        if self.warning is not None and self.warning not in output:
            return f"the log lacks {self.warning!r}{result.limit_note}"
        if self.warning is None and "using the default root" in output:
            return "the kernel warned about the root, which it should not"
        if self.number is None:
            expected = TMPFS_ROOT
        else:
            name = self.Name(self.number)
            if self.read_only:
                expected = (
                    f"KERN/W: VFS: / is {name} {self.RootType()}, "
                    f"{POLICY_REASON}"
                )
            else:
                expected = (
                    f"KERN/I: VFS: / is {name} {self.RootType()}, read-write"
                )
        if expected not in output:
            return f"the log lacks {expected!r}{result.limit_note}"
        if self.number is not None:
            etc = ROOT_ETC_RAM_COPY if self.read_only else ROOT_ETC_OWN
            if etc not in output:
                return f"the log lacks {etc!r}"
        if self.HasInit():
            if INIT_MESSAGE not in output:
                return "init did not start from the root"
        elif RESCUE_INIT_MESSAGE not in output:
            return "init did not start from the initramfs"
        if SHELL_BANNER not in output:
            return f"the shell did not start{result.limit_note}"
        if "KERN/W: Shutdown:" in output:
            return "the shutdown reported a problem"
        return None

    def HasInit(self):
        """Only the first partition of the test disk, and of the 4K image,
        is a root with programs."""
        return self.number == 1

    def CheckShell(self, output, result):
        patterns = []
        if self.number is None:
            name = self.Name(1)
            patterns.append(
                re.compile(rf"^{name} /mnt ext4 rw 0 0\r?$", re.MULTILINE)
            )
            patterns.append(re.compile(r"^BLOCK\.TXT\b.*$", re.MULTILINE))
            for number, (kind, start, end) in self.volumes.items():
                patterns.append(
                    re.compile(
                        rf"^{self.Name(number)} +{(end - start) // 2} "
                        rf"{SECTOR_SIZE} {kind}\r?$",
                        re.MULTILINE,
                    )
                )
            patterns.append(
                re.compile(rf"^{self.disk} +\d+ {SECTOR_SIZE} \w+\r?$", re.M)
            )
        elif self.read_only:
            patterns.append(re.compile(re.escape(REFUSED_WRITE), re.M))
        else:
            patterns.append(re.compile(r"^written on the root disk\r?$", re.M))
        for pattern in patterns:
            if pattern.search(output) is None:
                return (
                    f"the session never printed {pattern.pattern!r}"
                    f"{result.limit_note}"
                )
        if self.number is not None and not self.read_only:
            mount = re.compile(
                rf"^{self.Name(self.number)} / {self.RootType()} rw 0 0\r?$",
                re.MULTILINE,
            )
            if mount.search(output) is None:
                return "/proc/mounts does not list the root as read-write"
        for message in ("KERN/I: Shutdown: disk flushed", "Powering off"):
            if message not in output:
                return f"the log lacks {message!r}{result.limit_note}"
        if not result.exited_by_itself:
            return "QEMU kept running after shutdown"
        return None

    def CheckImage(self, result):
        if self.read_only:
            if Digest(self.image) != self.digest:
                return "the read-only root's image changed"
            return None
        number = 1 if self.number is None else self.number
        kind, start, end = self.volumes[number]
        if not kind.startswith("ext"):
            return None
        debugfs = FindE2fsprogs("debugfs")
        if debugfs is None:
            return "debugfs not found; install e2fsprogs"
        with tempfile.TemporaryDirectory() as work:
            volume = Path(work) / "root.img"
            volume.write_bytes(PartitionBytes(self.image, start, end))
            if NeedsRecovery(volume):
                return "the shutdown did not close the journal"
            if not IsMarkedClean(volume):
                return "the shutdown did not mark the volume clean"
            checked = E2fsck(str(volume))
            if checked is not None:
                return checked
            if self.number is not None:
                found = Debugfs(debugfs, str(volume), "cat /rootdisk.txt")
                if found != WRITTEN:
                    return "debugfs read a different /rootdisk.txt"
        return None

    def PassMessage(self, result):
        where = "RAM root" if self.number is None else self.Name(self.number)
        mode = "read-only, image unchanged" if self.read_only else "checked"
        return f"{where} on {self.bus} booted ({mode})" + (
            ", with the warning" if self.warning else ""
        )
