import tempfile
from pathlib import Path

from create_test_disk import EXT2_END, EXT2_START, FAT16_START, ROOT_START

from smoke.boot import PANIC_MARKER, SHELL_BANNER
from smoke.cases import storage
from smoke.cases.base import SHELL_TIMEOUT_SECONDS, Case
from smoke.script import Lines

POWER_MESSAGES = {
    "shutdown": (
        "KERN/I: Shutdown: stopping processes",
        "KERN/I: Shutdown: disk flushed",
        "KERN/I: Powering off",
    ),
    "reboot": (
        "KERN/I: Shutdown: stopping processes",
        "KERN/I: Shutdown: disk flushed",
        "KERN/I: Restarting",
    ),
}


# The ext4 root and the ext2 partition, which the script writes before the
# command.
POWER_VOLUMES = (
    ("root", ROOT_START, FAT16_START),
    ("ext2", EXT2_START, EXT2_END),
)


class Power(Case):
    timeout = SHELL_TIMEOUT_SECONDS
    serial_input = True

    def __init__(self, args):
        super().__init__(args)
        self.kind = args.expect_power
        self.reboots = self.kind == "reboot"
        self.shuts_down = self.kind == "shutdown"

    def Script(self, session):
        return Lines(
            b"mount -t ext2 vda6 /mnt\n"
            + f"write /mnt/power.txt before {self.kind}\n".encode()
            + f"write /power.txt before {self.kind}\n".encode()
            + f"{self.kind}\n".encode()
        )

    # A shutdown ends when QEMU exits; a reboot when the shell is back.
    def Markers(self):
        return (PANIC_MARKER.encode(),)

    def Finished(self, clean_output):
        return self.reboots and clean_output.count(SHELL_BANNER.encode()) >= 2

    def Check(self, result):
        output = result.output
        missing = [
            text for text in POWER_MESSAGES[self.kind] if text not in output
        ]
        boots = output.count(
            "KERN/I: Limine transferred control to the kernel"
        )
        if missing or PANIC_MARKER in output:
            return f"power output is missing {missing}{result.limit_note}"
        if self.shuts_down and not result.exited_by_itself:
            return "QEMU kept running after shutdown"
        if self.reboots and boots < 2:
            return "the system did not boot again"
        return self.VerifyVolumes()

    def VerifyVolumes(self):
        """The shutdown closed the root's journal and synced ext2, so both hold
        the files and e2fsck -fn finds nothing. A rebooted root was mounted
        read-write again, which sets needs_recovery until the next unmount, so
        its copy is replayed first."""
        debugfs = storage.FindE2fsprogs("debugfs")
        if debugfs is None:
            return "debugfs not found; install e2fsprogs"
        text = f"before {self.kind}\n".encode()
        for name, first, end in POWER_VOLUMES:
            with tempfile.TemporaryDirectory() as work:
                volume = Path(work) / f"{name}.img"
                volume.write_bytes(
                    storage.PartitionBytes(self.args.disk, first, end)
                )
                if name == "ext2" and not storage.IsMarkedClean(volume):
                    return "the ext2 partition was not marked clean"
                if name == "root" and self.shuts_down:
                    if storage.NeedsRecovery(volume):
                        return "the root's journal was not closed"
                    if not storage.IsMarkedClean(volume):
                        return "the root was not marked clean by the shutdown"
                elif name == "root":
                    replayed = storage.ReplayOnCopy(volume)
                    if replayed is not None:
                        return f"root: {replayed}"
                checked = storage.E2fsck(str(volume))
                if checked is not None:
                    return f"{name}: {checked}"
                found = storage.Debugfs(debugfs, str(volume), "cat /power.txt")
                if found != text:
                    return f"{name}: debugfs read a different /power.txt"
        return None

    def PassMessage(self, result):
        outcome = "powered off" if self.shuts_down else "booted again"
        return (
            f"{self.kind} stopped processes, closed the journals, flushed the "
            f"disk, and {outcome}"
        )
