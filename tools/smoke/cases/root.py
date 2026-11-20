import re
import subprocess
import tempfile
from pathlib import Path

from create_test_disk import (
    EXT2_END,
    EXT2_START,
    EXT3_START,
    FAT16_START,
    ROOT_START,
    SECTOR_SIZE,
)

from smoke.boot import PANIC_MARKER, RESCUE_INIT_MESSAGE, SHELL_BANNER
from smoke.cases import storage
from smoke.cases.base import SHELL_TIMEOUT_SECONDS, Case
from smoke.cases.normal_boot import NormalBoot
from smoke.script import Lines

# The partitions of the test disk that root= can name instead of the default
# root: the partition's range and its type. vda3 and vda4 boot as they are,
# with no /home, /bin, /sbin or /BLOCK.TXT; vda1 is made into a new ext4
# volume with nothing but lost+found first.
ROOT_VOLUMES = {
    "vda1": ("ext4", ROOT_START, FAT16_START),
    "vda3": ("ext3", EXT3_START, EXT2_START),
    "vda4": ("ext2", EXT2_START, EXT2_END),
}
EMPTY_ROOT = "vda1"
# The empty root's /sbin/init is garbage, so the kernel must start
# /system/init.
# Its /etc/init.d holds a file that does not parse, which init must skip, and a
# service that must still run; /etc/init.conf is no longer a default.
BROKEN_SERVICE = b"\xff\x00 not a service\nrespawn\n\x01\x02\n"
GOOD_SERVICE = (
    b"# root-service-marker\nonce grep root-service-marker /etc/init.d/good\n"
)
SERVICE_MARKER = "# root-service-marker"
# Every built-in console is disabled, so init must start a shell on tty1.
DISABLED_CONSOLES = b"/dev/tty1\n/dev/tty2\n/dev/tty3\n/dev/tty4\n"
NO_CONSOLE = "init: no console service; starting a shell on /dev/tty1"
SERVICE_SKIPPED = "init: /etc/init.d/broken: does not parse; skipped"
WRITTEN = b"written on the root\n"
WORKING_DIRECTORY = re.compile(r"^/\r?$", re.MULTILINE)
TMP_DIRECTORY = re.compile(r"^/tmp\r?$", re.MULTILINE)
HOME_IS_ROOT = re.compile(r"^HOME=/\r?$", re.MULTILINE)
TEST_FILES = ("BLOCK.TXT", "home", "bin", "sbin", "samples")
FALLBACK_WARNING = "KERN/W: VFS: root={} is not a usable partition, "


def RootOption(args):
    if args.expect_root in ROOT_VOLUMES:
        return NamedRoot(args)
    return UnusableRoot(args)


class NamedRoot(Case):
    """root=vdaN names an ext2, ext3 or ext4 partition: it is mounted
    read-write at /, the shell starts in / since the volume has no /home and
    writes a file, and shutdown closes the journal or marks the volume clean,
    which e2fsck and debugfs then check on a copy. The ext4 case is a volume
    just made by mke2fs."""

    timeout = SHELL_TIMEOUT_SECONDS
    serial_input = True
    shuts_down = True

    def __init__(self, args):
        super().__init__(args)
        self.name = args.expect_root
        self.type, self.first, self.end = ROOT_VOLUMES[self.name]

    def Start(self):
        if self.name != EMPTY_ROOT:
            return
        mke2fs = storage.FindE2fsprogs("mke2fs")
        if mke2fs is None:
            raise RuntimeError("mke2fs not found; install e2fsprogs")
        sectors = self.end - self.first
        with tempfile.TemporaryDirectory() as work:
            volume = Path(work) / "root.img"
            volume.write_bytes(bytes(sectors * SECTOR_SIZE))
            subprocess.run(
                [
                    mke2fs,
                    "-q",
                    "-F",
                    "-t",
                    "ext4",
                    "-b",
                    "1024",
                    "-I",
                    "256",
                    "-E",
                    "root_owner=0:0",
                    str(volume),
                ],
                capture_output=True,
                check=True,
            )
            self.AddServices(work, volume)
            with open(self.args.disk, "r+b") as disk:
                disk.seek(self.first * SECTOR_SIZE)
                disk.write(volume.read_bytes())

    def AddServices(self, work, volume):
        debugfs = storage.FindE2fsprogs("debugfs")
        if debugfs is None:
            raise RuntimeError("debugfs not found; install e2fsprogs")
        broken = Path(work) / "broken"
        broken.write_bytes(BROKEN_SERVICE)
        good = Path(work) / "good"
        good.write_bytes(GOOD_SERVICE)
        disabled = Path(work) / "disabled"
        disabled.write_bytes(DISABLED_CONSOLES)
        commands = Path(work) / "commands"
        commands.write_text(
            f"mkdir /etc\nmkdir /etc/init.d\n"
            f"write {disabled} /etc/init.disabled\n"
            f"write {broken} /etc/init.d/broken\n"
            f"write {good} /etc/init.d/good\n"
            f"mkdir /sbin\nwrite {broken} /sbin/init\n"
        )
        storage.RunDebugfsWrite(debugfs, str(volume), str(commands))

    def Script(self, session):
        return Lines(
            b"pwd\ncd /tmp\npwd\ncd\npwd\nenv\n"
            b"write /rootwrite.txt written on the root\n"
            b"cat /rootwrite.txt\n"
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
        if RESCUE_INIT_MESSAGE not in output:
            return "the kernel did not fall back to /system/init"
        if SHELL_BANNER not in output:
            return "the shell did not start"
        if WORKING_DIRECTORY.search(output) is None:
            return "the shell did not start in /"
        moved = TMP_DIRECTORY.search(output)
        if (
            moved is None
            or WORKING_DIRECTORY.search(output, moved.end()) is None
        ):
            return "cd without an argument did not go to / (HOME)"
        if HOME_IS_ROOT.search(output) is None:
            return "init did not set HOME=/ on a root without /home"
        broken = self.name == EMPTY_ROOT
        if broken != (SERVICE_SKIPPED in output):
            return (
                "init did not skip the /etc/init.d file that does not parse"
                if broken
                else "init skipped an /etc/init.d file on a root without any"
            )
        if broken != (SERVICE_MARKER in output):
            return (
                "init did not start the /etc/init.d service beside the "
                "built-in ones"
                if broken
                else "init started an /etc/init.d service on a root without"
            )
        if broken != (NO_CONSOLE in output):
            return (
                "init did not start a tty1 shell with every console disabled"
                if broken
                else "init added a shell with consoles configured"
            )
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
            listed = storage.Debugfs(debugfs, str(volume), "ls -p /").decode()
            names = {
                line.split("/")[5] for line in listed.splitlines() if line
            }
            if self.name == EMPTY_ROOT and names - {
                ".",
                "..",
                "etc",
                "lost+found",
                "rootwrite.txt",
                "sbin",
            }:
                return (
                    f"the new root holds more than it should: {sorted(names)}"
                )
            if self.name != EMPTY_ROOT and names.intersection(TEST_FILES):
                return "the root holds test files the boot should not need"
            for name in ("hosts", "resolv.conf"):
                expected = (storage.ETC_DEFAULTS / name).read_bytes()
                if (
                    storage.Debugfs(debugfs, str(volume), f"cat /etc/{name}")
                    != expected
                ):
                    return f"the root's /etc/{name} is not the default"
            if not storage.NotFound(debugfs, str(volume), "/etc/init.conf"):
                return "the root's /etc has an init.conf"
            if broken:
                for name, expected in (
                    ("broken", BROKEN_SERVICE),
                    ("good", GOOD_SERVICE),
                ):
                    if (
                        storage.Debugfs(
                            debugfs, str(volume), f"cat /etc/init.d/{name}"
                        )
                        != expected
                    ):
                        return f"the root's /etc/init.d/{name} changed"
        return None

    def PassMessage(self, result):
        return (
            f"{self.type} root {self.name} without test files mounted "
            "read-write with the defaults copied into its /etc, "
            "/system/init as its init, the shell "
            "started in / with HOME=/ and cd returning there, "
            + (
                "init skipping a broken /etc/init.d file, running the "
                "good one beside the built-in services, and the kernel "
                "starting /system/init over a broken /sbin/init, "
                if self.name == EMPTY_ROOT
                else ""
            )
            + "written from the shell, closed by shutdown, and clean under "
            "e2fsck and debugfs"
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
