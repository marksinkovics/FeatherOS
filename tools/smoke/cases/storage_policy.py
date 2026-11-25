import hashlib
import re
import shutil
from collections import Counter
from pathlib import Path

import create_storage_disks as storage_disks
from create_storage_disks import SECTOR_SIZE

from smoke.boot import PANIC_MARKER, ROOT_MESSAGE
from smoke.cases import ahci
from smoke.cases.base import SHELL_TIMEOUT_SECONDS, Case
from smoke.cases.nvme import (
    CONTROLLER_LINE,
    DISK_KIBIBYTES,
    KERNEL_MESSAGES,
    NVME,
    Present,
)
from smoke.script import Lines, WaitFor

VOLUMES = NVME.volumes + ahci.AHCI.volumes
NVME_POLICY_LINE = "KERN/I: Storage: nvme0: storage=ro, read-only"
AHCI_POLICY_LINE = "KERN/I: Storage: ahci0: storage=ro, read-only"
NVME_DISK_LINE = (
    f"KERN/I: Storage: nvme0n1: {DISK_KIBIBYTES} KiB, {SECTOR_SIZE}-byte "
    "sectors, ro"
)
AHCI_DISK_LINE = re.compile(
    rf"KERN/I: Storage: sda: AHCI {ahci.LOCATION} port 0, "
    rf"\"QEMU HARDDISK\" \"{ahci.SERIAL}\", {DISK_KIBIBYTES} KiB, "
    rf"{SECTOR_SIZE}-byte sectors, write cache, ro\r?$",
    re.MULTILINE,
)
STEPS = (
    "KERN/STORAGEPOLICY: nvme0n1 is read-only by the policy",
    "KERN/STORAGEPOLICY: no volume took a file",
    "KERN/STORAGEPOLICY: a write through the disk refused",
    "KERN/STORAGEPOLICY: no write or flush command was sent",
    "KERN/STORAGEPOLICY: sda is read-only by the policy",
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
    """An emulated NVMe controller and an ich9-ahci controller, each over its
    own copy of storage-controller.img, with storage=ro on the kernel command
    line, which makes every NVMe and AHCI disk read-only although QEMU's
    controllers would allow writes. The test hook checks for each disk that it
    reports the policy, that a read-write mount of each volume takes no file,
    that a write through the disk is refused and that the controller's
    counters of write and flush commands stay zero. The shell checks each
    mount shows ro, that a write fails with EROFS and that mount -o
    remount,rw fails too, and the harness checks both images' SHA-256 is
    unchanged."""

    timeout = SHELL_TIMEOUT_SECONDS
    serial_input = True
    shuts_down = True
    passed = (
        "storage=ro kept the NVMe and AHCI disks read-only through the VFS "
        "and the disk, no write or flush command was sent, and both images "
        "are unchanged"
    )

    def __init__(self, args):
        super().__init__(args)
        self.directory = Path(args.disk).parent / "storage-policy-disks"
        self.nvme_image = None
        self.ahci_image = None
        self.digests = {}

    def Start(self):
        shutil.rmtree(self.directory, ignore_errors=True)
        self.nvme_image = storage_disks.BuildControllerDisk(
            self.directory / "nvme"
        )
        self.ahci_image = storage_disks.BuildControllerDisk(
            self.directory / "ahci"
        )
        self.digests = {
            "NVMe": (self.nvme_image, Digest(self.nvme_image)),
            "AHCI": (self.ahci_image, Digest(self.ahci_image)),
        }

    def QemuArguments(self):
        return [
            "-drive",
            f"if=none,id=nvme_disk,file={self.nvme_image},format=raw",
            "-device",
            "nvme,serial=feather-nvme0,drive=nvme_disk",
            *ahci.QemuArguments(self.ahci_image),
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
        if ahci.CONTROLLER_LINE.search(output) is None:
            return "the boot log lacks the AHCI controller line"
        if ahci.ATAPI_LINE.search(output) is None:
            return "the log lacks the refusal of the ATAPI device on port 2"
        for line in (
            NVME_POLICY_LINE,
            AHCI_POLICY_LINE,
            NVME_DISK_LINE,
            AHCI_DISK_LINE,
            ROOT_MESSAGE,
        ):
            if not Present(output, line):
                return f"the log lacks '{getattr(line, 'pattern', line)}'"
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
        for naming in (NVME, ahci.AHCI):
            pattern = re.compile(
                rf"^{naming.disk} +{DISK_KIBIBYTES} {SECTOR_SIZE} "
                rf"{naming.kind} ro\r?$",
                re.MULTILINE,
            )
            if pattern.search(output) is None:
                return f"/proc/partitions does not show {naming.disk} as ro"
        for source, _, _, _ in VOLUMES:
            line = (
                f"VFS: {source} is read-only by the storage policy "
                "(storage=rw to write), mounted read-only"
            )
            if line not in output:
                return f"the log lacks '{line}'"
        for line, count in Counter(ShellOutput()).items():
            found = re.findall(
                rf"^{re.escape(line)}\r?$", output, re.MULTILINE
            )
            if len(found) < count:
                return (
                    f"the shell session printed '{line}' {len(found)} times, "
                    f"not {count}{result.limit_note}"
                )
        for message in KERNEL_MESSAGES:
            if message not in output:
                return f"the log lacks '{message}'{result.limit_note}"
        if not result.exited_by_itself:
            return "QEMU kept running after shutdown"
        return None

    def CheckImage(self):
        for name, (image, digest) in self.digests.items():
            if Digest(image) != digest:
                return f"the {name} image changed under storage=ro"
        return None
