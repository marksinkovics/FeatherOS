import tempfile
from pathlib import Path

from create_test_disk import EXT2_END, EXT2_START, EXT3_START, SECTOR_SIZE

from smoke.boot import PANIC_MARKER, SHELL_BANNER
from smoke.cases import storage
from smoke.cases.base import SHELL_TIMEOUT_SECONDS, Case
from smoke.cases.normal_boot import NormalBoot
from smoke.script import Lines

BLOCK_FILE = Path(__file__).resolve().parents[3] / "initramfs" / "block.txt"
# The partitions of the test disk that root= can name instead of the default
# root: the partition's range and its type. Their root files are added before
# the boot, since the kernel reads /BLOCK.TXT from whatever root it mounts.
ROOT_VOLUMES = {
    "vda3": ("ext3", EXT3_START, EXT2_START),
    "vda4": ("ext2", EXT2_START, EXT2_END),
}
ROOT_DIRECTORIES = ("home", "bin")
WRITTEN = b"written on the root\n"
FALLBACK_WARNING = "KERN/W: VFS: root={} is not a usable partition, "


def RootOption(args):
    if args.expect_root in ROOT_VOLUMES:
        return NamedRoot(args)
    return UnusableRoot(args)


class NamedRoot(Case):
    """root=vdaN names an ext2 or ext3 partition: it is mounted read-write at
    /, the shell writes a file, and shutdown closes the journal or marks the
    volume clean, which e2fsck and debugfs then check on a copy."""

    timeout = SHELL_TIMEOUT_SECONDS
    serial_input = True
    shuts_down = True

    def __init__(self, args):
        super().__init__(args)
        self.name = args.expect_root
        self.type, self.first, self.end = ROOT_VOLUMES[self.name]

    def Start(self):
        debugfs = storage.FindE2fsprogs("debugfs")
        if debugfs is None:
            raise RuntimeError("debugfs not found; install e2fsprogs")
        with tempfile.TemporaryDirectory() as work:
            volume = Path(work) / "root.img"
            commands = Path(work) / "commands"
            volume.write_bytes(
                storage.PartitionBytes(self.args.disk, self.first, self.end)
            )
            commands.write_text(
                f"write {BLOCK_FILE} BLOCK.TXT\n"
                + "".join(f"mkdir {name}\n" for name in ROOT_DIRECTORIES)
            )
            storage.RunDebugfsWrite(debugfs, str(volume), str(commands))
            with open(self.args.disk, "r+b") as disk:
                disk.seek(self.first * SECTOR_SIZE)
                disk.write(volume.read_bytes())

    def Script(self, session):
        return Lines(
            b"write /rootwrite.txt written on the root\n"
            b"cat /BLOCK.TXT\n"
            b"shutdown\n"
        )

    def Markers(self):
        return (PANIC_MARKER.encode(),)

    def Check(self, result):
        output = result.output
        expected = f"KERN/I: VFS: / is {self.name} {self.type}, read-write"
        if expected not in output:
            return f"expected {expected!r}{result.limit_note}"
        if PANIC_MARKER in output:
            return "the kernel panicked"
        if SHELL_BANNER not in output:
            return "the shell did not start"
        if "KERN/I: Powering off" not in output:
            return f"the system did not power off{result.limit_note}"
        if not result.exited_by_itself:
            return "QEMU kept running after shutdown"
        debugfs = storage.FindE2fsprogs("debugfs")
        if debugfs is None:
            return "debugfs not found; install e2fsprogs"
        with tempfile.TemporaryDirectory() as work:
            volume = Path(work) / "root.img"
            volume.write_bytes(
                storage.PartitionBytes(self.args.disk, self.first, self.end)
            )
            if storage.NeedsRecovery(volume):
                return "the shutdown did not close the root's journal"
            if not storage.IsMarkedClean(volume):
                return "the shutdown did not mark the root clean"
            checked = storage.E2fsck(str(volume))
            if checked is not None:
                return checked
            found = storage.Debugfs(debugfs, str(volume), "cat /rootwrite.txt")
            if found != WRITTEN:
                return "debugfs read a different /rootwrite.txt"
        return None

    def PassMessage(self, result):
        return (
            f"{self.type} root {self.name} mounted read-write, written from "
            "the shell, closed by shutdown, and clean under e2fsck and debugfs"
        )


class UnusableRoot(NormalBoot):
    """A root= that names no usable partition warns and boots the default
    root."""

    def Check(self, result):
        warning = FALLBACK_WARNING.format(self.args.expect_root)
        if warning not in result.output:
            return f"expected the warning {warning!r}"
        return super().Check(result)

    def PassMessage(self, result):
        return (
            f"root={self.args.expect_root} warned and the default root booted"
        )
