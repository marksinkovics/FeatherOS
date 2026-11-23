import re
import shutil
import tempfile
from pathlib import Path

import create_storage_disks as storage_disks
import create_usb_disks as usb_disks
from create_storage_disks import SECTOR_SIZE

from smoke.boot import PANIC_MARKER, ROOT_MESSAGE
from smoke.cases.base import SHELL_TIMEOUT_SECONDS, Case
from smoke.cases.nvme import CONTROLLER_LINE, KERNEL_MESSAGES
from smoke.disk_checks import (
    Debugfs,
    E2fsck,
    FatCopiesAgree,
    FatStatusWord,
    FindE2fsprogs,
    FindFsckFat,
    FsckFat,
    IsMarkedClean,
    MtoolsSpec,
    Mtype,
    NeedsRecovery,
    PartitionBytes,
)
from smoke.script import Lines, WaitFor

FOURK = usb_disks.FOURK_SECTOR_SIZE
FOURK_KIBIBYTES = usb_disks.FOURK_BYTES // 1024
FOURK_EXT4_KIBIBYTES = usb_disks.FOURK_EXT4_SECTORS * FOURK // 1024
FLAT_KIBIBYTES = storage_disks.NAMESPACE_512_SECTORS * SECTOR_SIZE // 1024
FOURK_NOTE = b"written on 4096-byte sectors\n"
FLAT_NOTE = b"written on the 512-byte namespace\n"
DISK_LINES = (
    f"KERN/I: Storage: nvme0n1: {FOURK_KIBIBYTES} KiB, {FOURK}-byte "
    "sectors, rw",
    f"KERN/I: Storage: nvme0n2: {FLAT_KIBIBYTES} KiB, {SECTOR_SIZE}-byte "
    "sectors, rw",
)
VOLUME_LINES = (
    f"KERN/I: Storage: nvme0n1p5: ext4, {FOURK_EXT4_KIBIBYTES} KiB",
    f"KERN/I: Storage: nvme0n2: fat16, {FLAT_KIBIBYTES} KiB",
)
REFUSED_FAT16 = "Storage: nvme0n1p6: no supported filesystem"
METADATA_REFUSAL = (
    "Storage: nvme0: namespace 3: LBA format with metadata not supported"
)
SHELL_INPUT = b"""sleep 1
cat /proc/partitions
mkdir /tmp/ns
mount -t ext4 nvme0n1p5 /tmp/ns
cat /tmp/ns/FOURK.TXT
write /tmp/ns/w4k.txt written on 4096-byte sectors
cp /samples/extents.bin /tmp/ns/extents.bin
cat /tmp/ns/w4k.txt
sync
umount /tmp/ns
mount -t fat16 nvme0n1p6 /tmp/ns
echo refused fat16 done
mount -t fat16 nvme0n2 /tmp/ns
cat /tmp/ns/NS512.TXT
write /tmp/ns/NOTE.TXT written on the 512-byte namespace
cat /tmp/ns/NOTE.TXT
sync
umount /tmp/ns
shutdown
"""
SHELL_OUTPUT = (
    "ext4 on 4096-byte sectors",
    "written on 4096-byte sectors",
    "refused fat16 done",
    "fat16 on a 512-byte namespace",
    "written on the 512-byte namespace",
)


class NvmeNamespaces(Case):
    """One emulated NVMe controller with three nvme-ns namespaces built by
    create_storage_disks.py: nsid 1 in 4096-byte sectors (GPT, an ext4 and a
    FAT16 partition, which the FAT16 core refuses), nsid 2 a 512-byte FAT16
    superfloppy, and nsid 3 with 8 bytes of metadata per block, which the
    driver refuses. The shell writes both usable namespaces, and the harness
    checks the images with e2fsck and mtools. The case fails when the log
    shows a sector size other than the one asked for or lacks the refusal
    (QEMU ignoring a property)."""

    timeout = SHELL_TIMEOUT_SECONDS
    serial_input = True
    shuts_down = True
    passed = (
        "a 4096-byte and a 512-byte namespace were probed, written and "
        "read back, the namespace with metadata was refused, and the images "
        "passed the host checks"
    )

    def __init__(self, args):
        super().__init__(args)
        self.directory = Path(args.build_dir) / "storage-namespaces"
        self.images = {}
        self.fourk_fat16 = b""
        self.fsck_note = ""
        self.interrupts = "unknown"

    def Start(self):
        shutil.rmtree(self.directory, ignore_errors=True)
        self.images = storage_disks.BuildNamespaceDisks(self.directory)
        self.fourk_fat16 = self.FourKFat16Bytes()

    def FourKFat16Bytes(self):
        scale = FOURK // SECTOR_SIZE
        return PartitionBytes(
            self.images["ns4k"],
            usb_disks.FOURK_FAT16_START * scale,
            (usb_disks.FOURK_FAT16_START + usb_disks.FOURK_FAT16_SECTORS)
            * scale,
        )

    def QemuArguments(self):
        arguments = ["-device", "nvme,id=nvme0,serial=feather-nvme0"]
        for nsid, key, extra in (
            (1, "ns4k", "logical_block_size=4096,physical_block_size=4096"),
            (2, "ns512", "logical_block_size=512"),
            (3, "nsms", "logical_block_size=512,ms=8"),
        ):
            arguments += [
                "-drive",
                f"if=none,id=ns{nsid},file={self.images[key]},format=raw",
                "-device",
                f"nvme-ns,bus=nvme0,nsid={nsid},drive=ns{nsid},{extra}",
            ]
        return arguments

    def Script(self, session):
        return [WaitFor(VOLUME_LINES[1]), *Lines(SHELL_INPUT)]

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
            or self.CheckShell(output, result)
            or self.CheckFourK()
            or self.CheckFlat()
            or self.CheckMetadata()
        )

    def CheckBoot(self, output):
        found = CONTROLLER_LINE.search(output)
        if found is None:
            return "the boot log lacks the NVMe controller line"
        self.interrupts = found.group(1)
        if ROOT_MESSAGE not in output:
            return f"the log lacks '{ROOT_MESSAGE}'"
        for line in (*DISK_LINES, *VOLUME_LINES):
            pattern = re.compile(rf"^{re.escape(line)}\r?$", re.MULTILINE)
            if pattern.search(output) is None:
                return (
                    f"the log lacks '{line}' (QEMU ignored a property, or "
                    "the namespace was read wrongly)"
                )
        if METADATA_REFUSAL not in output:
            return (
                "the log lacks the refusal of the namespace with metadata "
                "(QEMU ignored ms=8?)"
            )
        if "nvme0n3" in output:
            return "the namespace with metadata was registered as nvme0n3"
        if REFUSED_FAT16 not in output:
            return "the 4096-byte namespace's FAT16 partition was not refused"
        return None

    def CheckShell(self, output, result):
        for name, kibibytes, sector in (
            ("nvme0n1", FOURK_KIBIBYTES, FOURK),
            ("nvme0n2", FLAT_KIBIBYTES, SECTOR_SIZE),
        ):
            pattern = re.compile(
                rf"^{name} +{kibibytes} {sector} nvme\r?$", re.MULTILINE
            )
            if pattern.search(output) is None:
                return f"/proc/partitions lacks the disk line of {name}"
        for line in SHELL_OUTPUT:
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

    def CheckFourK(self):
        scale = FOURK // SECTOR_SIZE
        volume_bytes = PartitionBytes(
            self.images["ns4k"],
            usb_disks.FOURK_EXT4_START * scale,
            (usb_disks.FOURK_EXT4_START + usb_disks.FOURK_EXT4_SECTORS)
            * scale,
        )
        debugfs = FindE2fsprogs("debugfs")
        if debugfs is None:
            return "debugfs not found; install e2fsprogs"
        with tempfile.TemporaryDirectory() as work:
            volume = Path(work) / "ext4.img"
            volume.write_bytes(volume_bytes)
            if not IsMarkedClean(volume) or NeedsRecovery(volume):
                return "nvme0n1p5 is not clean after the umount"
            checked = E2fsck(str(volume))
            if checked is not None:
                return f"nvme0n1p5: {checked}"
            for name, contents in (
                ("/FOURK.TXT", usb_disks.FOURK_CONTENTS),
                ("/w4k.txt", FOURK_NOTE),
            ):
                if Debugfs(debugfs, str(volume), f"cat {name}") != contents:
                    return f"nvme0n1p5: debugfs read a different {name}"
            copied = Path(work) / "extents.bin"
            Debugfs(debugfs, str(volume), f"dump /extents.bin {copied}")
            if not copied.exists() or copied.stat().st_size < 1024 * 1024:
                return "nvme0n1p5: the copy of extents.bin is missing or short"
        if self.FourKFat16Bytes() != self.fourk_fat16:
            return "nvme0n1p6: the refused FAT16 partition was written"
        return None

    def CheckFlat(self):
        image = self.images["ns512"]
        if not FatCopiesAgree(image):
            return "nvme0n2: the FAT copies differ"
        spec = MtoolsSpec(image)
        for name, contents in (
            (
                storage_disks.NAMESPACE_512_FILE,
                storage_disks.NAMESPACE_512_CONTENTS,
            ),
            ("NOTE.TXT", FLAT_NOTE),
        ):
            if Mtype(spec, name) != contents:
                return f"nvme0n2: mtools read different {name} contents"
        if FatStatusWord(image) != 0xFFFF:
            return "nvme0n2: FAT[1] changed"
        checked = FsckFat(image, 0, storage_disks.NAMESPACE_512_SECTORS)
        if checked is not None:
            return f"nvme0n2: {checked}"
        if FindFsckFat() is None:
            self.fsck_note = "; fsck.fat is not installed, skipped"
        return None

    def CheckMetadata(self):
        if any(self.images["nsms"].read_bytes()):
            return "nvme0n3: the refused namespace was written"
        return None

    def PassMessage(self, result):
        return f"{self.passed} (interrupts: {self.interrupts}){self.fsck_note}"
