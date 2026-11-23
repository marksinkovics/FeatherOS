import hashlib
import re
import shutil
from pathlib import Path

import create_storage_disks as storage_disks
from create_storage_disks import SECTOR_SIZE

from smoke.boot import PANIC_MARKER, ROOT_MESSAGE
from smoke.cases.base import SHELL_TIMEOUT_SECONDS, Case
from smoke.cases.nvme import (
    CONTROLLER_LINE,
    DISK_KIBIBYTES,
    KERNEL_MESSAGES,
    VOLUMES,
)
from smoke.script import Lines, WaitFor

CONTROLLER_POLICY_LINE = "KERN/I: Storage: nvme0: storage=ro, read-only"
DISK_LINE = (
    f"KERN/I: Storage: nvme0n1: {DISK_KIBIBYTES} KiB, {SECTOR_SIZE}-byte "
    "sectors, ro"
)
STEPS = (
    "KERN/STORAGEPOLICY: nvme0n1 is read-only by the policy",
    "KERN/STORAGEPOLICY: no volume took a file",
    "KERN/STORAGEPOLICY: a write through the disk refused",
    "KERN/STORAGEPOLICY: no write or flush command was sent",
    "KERN/STORAGEPOLICY: tests passed",
)


def VolumeScript(source, kind):
    return [
        f"mount -t {kind} {source} /tmp/p",
        f"grep {source} /proc/mounts",
        f"write /tmp/p/f.txt refused {source}",
        "mount -o remount,rw /tmp/p",
        f"write /tmp/p/f.txt refused again {source}",
        "umount /tmp/p",
    ]


def ShellInput():
    lines = ["cat /proc/partitions", "mkdir /tmp/p"]
    for source, kind, _, _ in VOLUMES:
        lines += VolumeScript(source, kind)
    lines.append("shutdown")
    return "\n".join(lines) + "\n"


def ShellOutput():
    expected = []
    for source, kind, _, _ in VOLUMES:
        expected += [
            f"{source} /tmp/p {kind} ro 0 0",
            "write: cannot open /tmp/p/f.txt: Read-only file system",
            "mount: /tmp/p: Read-only file system",
        ]
    return tuple(expected)


def Digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class StoragePolicy(Case):
    """An emulated NVMe controller over storage-controller.img with
    storage=ro on the kernel command line, which makes every NVMe disk
    read-only although QEMU's controller would allow writes. The test hook
    checks that the disk reports the policy, that a read-write mount of each
    volume takes no file, that a write through the disk is refused and that
    the controller's counters of write and flush commands stay zero. The
    shell checks each mount shows ro, that a write fails with EROFS and that
    mount -o remount,rw fails too, and the harness checks the image's SHA-256
    is unchanged."""

    timeout = SHELL_TIMEOUT_SECONDS
    serial_input = True
    shuts_down = True
    passed = (
        "storage=ro kept the NVMe disk read-only through the VFS and the "
        "disk, no write or flush command was sent, and the image is unchanged"
    )

    def __init__(self, args):
        super().__init__(args)
        self.directory = Path(args.build_dir) / "storage-policy-disks"
        self.image = None
        self.digest = ""

    def Start(self):
        shutil.rmtree(self.directory, ignore_errors=True)
        self.image = storage_disks.BuildControllerDisk(self.directory)
        self.digest = Digest(self.image)

    def QemuArguments(self):
        return [
            "-drive",
            f"if=none,id=nvme_disk,file={self.image},format=raw",
            "-device",
            "nvme,serial=feather-nvme0,drive=nvme_disk",
        ]

    def Script(self, session):
        return [
            WaitFor("KERN/STORAGEPOLICY: tests passed"),
            *Lines(ShellInput().encode()),
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
        if CONTROLLER_LINE.search(output) is None:
            return "the boot log lacks the NVMe controller line"
        for line in (CONTROLLER_POLICY_LINE, DISK_LINE, ROOT_MESSAGE):
            if line not in output:
                return f"the log lacks '{line}'"
        if "writes allowed" in output:
            return "the controller line says writes are allowed"
        return None

    def CheckHook(self, output):
        position = 0
        for step in STEPS:
            found = output.find(step, position)
            if found < 0:
                return f"the test hook never reported '{step}' in order"
            position = found
        return None

    def CheckShell(self, output, result):
        pattern = re.compile(
            rf"^nvme0n1 +{DISK_KIBIBYTES} {SECTOR_SIZE} nvme ro\r?$",
            re.MULTILINE,
        )
        if pattern.search(output) is None:
            return "/proc/partitions does not show nvme0n1 as ro"
        for source, _, _, _ in VOLUMES:
            line = (
                f"VFS: {source} is read-only by the storage policy "
                "(storage=rw to write), mounted read-only"
            )
            if line not in output:
                return f"the log lacks '{line}'"
        for line in ShellOutput():
            if (
                re.search(rf"^{re.escape(line)}\r?$", output, re.MULTILINE)
                is None
            ):
                return (
                    f"the shell session never printed '{line}'"
                    f"{result.limit_note}"
                )
        for message in KERNEL_MESSAGES:
            if message not in output:
                return f"the log lacks '{message}'{result.limit_note}"
        if not result.exited_by_itself:
            return "QEMU kept running after shutdown"
        return None

    def CheckImage(self):
        if Digest(self.image) != self.digest:
            return "the NVMe image changed under storage=ro"
        return None
