import hashlib
import re
import shutil
import tempfile
from pathlib import Path

import create_usb_disks as usb_disks
from create_usb_disks import SECTOR_SIZE

from smoke.boot import PANIC_MARKER
from smoke.cases.base import SHELL_TIMEOUT_SECONDS, Case, SkipCase
from smoke.disk_checks import (
    Debugfs,
    E2fsck,
    FatCopiesAgree,
    FindE2fsprogs,
    IsMarkedClean,
    MtoolsSpec,
    Mtype,
    NeedsRecovery,
    PartitionBytes,
)
from smoke.script import Lines, WaitFor

HUB_IMAGE = "usb-hub.img"
LARGE_KIBIBYTES = usb_disks.LARGE_BYTES // 1024
LARGE_FAT16_KIBIBYTES = usb_disks.LARGE_FAT16_SECTORS * SECTOR_SIZE // 1024
FOURK_KIBIBYTES = usb_disks.FOURK_BYTES // 1024
FOURK_EXT4_KIBIBYTES = (
    usb_disks.FOURK_EXT4_SECTORS * usb_disks.FOURK_SECTOR_SIZE // 1024
)
HUB_IMAGE_KIBIBYTES = usb_disks.FAT16_IMAGE_SECTORS * SECTOR_SIZE // 1024
HUB_FAT16_KIBIBYTES = usb_disks.FAT16_SECTORS * SECTOR_SIZE // 1024
HUB_EXT2_KIBIBYTES = usb_disks.EXT2_SECTORS * SECTOR_SIZE // 1024
FOURK_BLOCK = usb_disks.FOURK_SECTOR_SIZE
LARGE_NOTE = b"written past 2 TiB\n"
FOURK_NOTE = b"written on 4096-byte sectors\n"
HUB_NOTE = b"written behind a hub\n"
EXTENTS_FILE = "/samples/extents.bin"
# The drives sit on root ports 1 and 2 and a hub on QEMU's USB 2 port 3,
# which the xHCI controller makes root port 7; the storage worker probes them
# in port order, so the letters follow.
DEVICES = (
    ("1-1", "super", "sda", LARGE_KIBIBYTES, SECTOR_SIZE),
    ("1-2", "super", "sdb", FOURK_KIBIBYTES, FOURK_BLOCK),
    (r"1-7\.1", "full", "sdc", HUB_IMAGE_KIBIBYTES, SECTOR_SIZE),
)
VOLUME_LINES = (
    ("sda1", "fat16", LARGE_FAT16_KIBIBYTES),
    ("sdb5", "ext4", FOURK_EXT4_KIBIBYTES),
    ("sdc1", "fat16", HUB_FAT16_KIBIBYTES),
    ("sdc2", "ext2", HUB_EXT2_KIBIBYTES),
)
FAILURES = (
    "reset recovery",
    "could not",
    "enumeration failed",
    "driver could not attach",
)
SHELL_INPUT = b"""sleep 1
cat /proc/partitions
mkdir /tmp/large
mount -t fat16 sda1 /tmp/large
cat /tmp/large/LARGE.TXT
write /tmp/large/w.txt written past 2 TiB
cat /tmp/large/w.txt
sync
umount /tmp/large
mount -t fat16 sda1 /tmp/large
cat /tmp/large/w.txt
umount /tmp/large
mkdir /tmp/fourk
mount -t ext4 sdb5 /tmp/fourk
cat /tmp/fourk/FOURK.TXT
write /tmp/fourk/w4k.txt written on 4096-byte sectors
cp /samples/extents.bin /tmp/fourk/extents.bin
cat /tmp/fourk/w4k.txt
sync
umount /tmp/fourk
mount -t fat16 sdb6 /tmp/fourk
echo refused fat16 done
mkdir /tmp/hub
mount -t fat16 sdc1 /tmp/hub
cat /tmp/hub/USBFILE.TXT
write /tmp/hub/hub.txt written behind a hub
cp /tmp/hub/BIG.BIN /tmp/hub/COPY.BIN
cat /tmp/hub/hub.txt
sync
umount /tmp/hub
shutdown
"""
SHELL_OUTPUT = (
    "fat16 past 2 TiB",
    "written past 2 TiB",
    "ext4 on 4096-byte sectors",
    "written on 4096-byte sectors",
    "refused fat16 done",
    "fat16 on a stick",
    "written behind a hub",
)


def Digest(data):
    return hashlib.sha256(data).hexdigest()


class UsbStorageLarge(Case):
    """Three QEMU usb-storage drives on the xHCI controller, built by
    create_usb_disks.py: sda, a sparse 3 TiB GPT drive whose FAT16 partition
    starts at 2.5 TiB (READ CAPACITY(16), READ(16) and WRITE(16)); sdb, 64 MiB
    in 4096-byte sectors (logical_block_size=4096) with an ext4 and a FAT16
    partition, which the FAT16 core refuses; sdc, an MBR drive behind QEMU's
    usb-hub, which is a full-speed device. The shell mounts, writes and reads
    each, and the harness checks the images with mtools and e2fsck. The case
    fails when the log shows a capacity, sector size or speed other than the
    one asked for (QEMU ignoring a property), and is skipped when the host
    cannot hold the 3 TiB image as a sparse file."""

    timeout = SHELL_TIMEOUT_SECONDS
    serial_input = True
    shuts_down = True
    passed = (
        "a 3 TiB drive, a 4096-byte-sector drive and a drive behind a hub "
        "were probed at the capacities, sector sizes and speeds asked for, "
        "written and read back, and the images passed the host checks"
    )

    def __init__(self, args):
        super().__init__(args)
        self.directory = Path(args.disk).parent / "usb-large-disks"
        self.images = {}
        self.fourk_fat16 = b""
        self.fsck_note = ""

    def Start(self):
        shutil.rmtree(self.directory, ignore_errors=True)
        self.directory.mkdir(parents=True)
        self.images["sdb"] = usb_disks.BuildFourK(self.directory)
        self.images["sdc"] = self.directory / HUB_IMAGE
        usb_disks.BuildPartitioned(self.images["sdc"])
        try:
            self.images["sda"] = usb_disks.BuildLarge(self.directory)
        except usb_disks.ImageUnavailable as error:
            shutil.rmtree(self.directory, ignore_errors=True)
            raise SkipCase(str(error)) from error
        self.fourk_fat16 = self.FourKFat16Bytes()

    def FourKFat16Bytes(self):
        scale = usb_disks.FOURK_SECTOR_SIZE // SECTOR_SIZE
        return PartitionBytes(
            self.images["sdb"],
            usb_disks.FOURK_FAT16_START * scale,
            (usb_disks.FOURK_FAT16_START + usb_disks.FOURK_FAT16_SECTORS)
            * scale,
        )

    def QemuArguments(self):
        drive = "if=none,id=usb{n},file={file},format=raw"
        return [
            "-device",
            "qemu-xhci,id=xhci",
            "-drive",
            drive.format(n=1, file=self.images["sda"]),
            "-device",
            "usb-storage,bus=xhci.0,port=1,drive=usb1,removable=on",
            "-drive",
            drive.format(n=2, file=self.images["sdb"]),
            "-device",
            "usb-storage,bus=xhci.0,port=2,drive=usb2,removable=on,"
            "logical_block_size=4096,physical_block_size=4096",
            "-device",
            "usb-hub,bus=xhci.0,port=3,id=hub1",
            "-drive",
            drive.format(n=3, file=self.images["sdc"]),
            "-device",
            "usb-storage,bus=xhci.0,port=3.1,drive=usb3,removable=on",
        ]

    def Script(self, session):
        return [WaitFor("Storage: sdc2: ext2, "), *Lines(SHELL_INPUT)]

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
            or self.CheckLarge()
            or self.CheckFourK()
            or self.CheckHub()
        )

    def CheckBoot(self, output):
        for path, speed, letter, kibibytes, sector in DEVICES:
            speed_line = re.compile(
                rf"KERN/I: USB {path}: {speed}-speed 46f4:0001 "
                r"\"QEMU USB HARDDRIVE\" usb-storage\r?$",
                re.MULTILINE,
            )
            if speed_line.search(output) is None:
                return (
                    f"the boot log lacks the {speed}-speed line of the "
                    f"drive at USB {path} (QEMU ignored a property?)"
                )
            probe = re.compile(
                rf"KERN/I: Storage: {letter}: USB {path}, \"QEMU\" "
                rf"\"QEMU HARDDISK\", {kibibytes} KiB, {sector}-byte "
                r"sectors, removable\r?$",
                re.MULTILINE,
            )
            if probe.search(output) is None:
                return (
                    f"the probe did not log {letter} with {kibibytes} KiB "
                    f"and {sector}-byte sectors (QEMU ignored a property, or "
                    "the capacity was read wrongly)"
                )
        if (
            'KERN/I: USB 1-7: full-speed 0409:55aa "QEMU USB Hub" hub'
            not in output
        ):
            return "the boot log lacks the full-speed line of the hub"
        for source, kind, kibibytes in VOLUME_LINES:
            pattern = re.compile(
                rf"KERN/I: Storage: {source}: {kind}, {kibibytes} KiB\r?$",
                re.MULTILINE,
            )
            if pattern.search(output) is None:
                return f"the worker did not log the volume {source}"
        if "Storage: sdb6: no supported filesystem" not in output:
            return "the FAT16 partition of the 4096-byte drive was not refused"
        kernel_lines = [
            line for line in output.splitlines() if "KERN/" in line
        ]
        for warning in FAILURES:
            if any(warning in line for line in kernel_lines):
                return f"the log reports a USB storage failure: {warning}"
        return None

    def CheckShell(self, output, result):
        listing = (
            ("sda", LARGE_KIBIBYTES, SECTOR_SIZE),
            ("sdb", FOURK_KIBIBYTES, FOURK_BLOCK),
        )
        for letter, kibibytes, sector in listing:
            pattern = re.compile(
                rf"^{letter} +{kibibytes} {sector} usb removable rw\r?$",
                re.MULTILINE,
            )
            if pattern.search(output) is None:
                return f"/proc/partitions lacks the disk line of {letter}"
        for line in SHELL_OUTPUT:
            if (
                re.search(rf"^{re.escape(line)}\r?$", output, re.MULTILINE)
                is None
            ):
                return (
                    f"the shell session never printed '{line}'"
                    f"{result.limit_note}"
                )
        if not result.exited_by_itself:
            return "QEMU kept running after shutdown"
        return None

    def CheckLarge(self):
        image = self.images["sda"]
        start = usb_disks.LARGE_FAT16_START
        if not FatCopiesAgree(image, start):
            return "sda1: the FAT copies differ"
        spec = MtoolsSpec(image, start)
        for name, contents in (
            (usb_disks.LARGE_FILE, usb_disks.LARGE_CONTENTS),
            ("w.txt", LARGE_NOTE),
        ):
            if Mtype(spec, name) != contents:
                return f"sda1: mtools read different {name} contents"
        low = PartitionBytes(image, 2048, 2048 + 8)
        if any(low):
            return "sda: the guest wrote below the partition (wrapped LBA?)"
        if usb_disks.AllocatedBytes(image) > (
            usb_disks.LARGE_MAXIMUM_ALLOCATED * 2
        ):
            return "the 3 TiB image is no longer sparse: a wrapped transfer?"
        return None

    def CheckFourK(self):
        image = self.images["sdb"]
        scale = FOURK_BLOCK // SECTOR_SIZE
        volume_bytes = PartitionBytes(
            image,
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
                return "sdb5 is not clean after the umount"
            checked = E2fsck(str(volume))
            if checked is not None:
                return f"sdb5: {checked}"
            for name, contents in (
                ("/FOURK.TXT", usb_disks.FOURK_CONTENTS),
                ("/w4k.txt", FOURK_NOTE),
            ):
                if Debugfs(debugfs, str(volume), f"cat {name}") != contents:
                    return f"sdb5: debugfs read a different {name}"
            copied = Path(work) / "extents.bin"
            Debugfs(debugfs, str(volume), f"dump /extents.bin {copied}")
            if not copied.exists() or copied.stat().st_size < 1024 * 1024:
                return "sdb5: the copy of extents.bin is missing or short"
        if self.FourKFat16Bytes() != self.fourk_fat16:
            return "sdb6: the refused FAT16 partition was written"
        return None

    def CheckHub(self):
        image = self.images["sdc"]
        if not FatCopiesAgree(image, usb_disks.FAT16_START):
            return "sdc1: the FAT copies differ"
        spec = MtoolsSpec(image, usb_disks.FAT16_START)
        big = usb_disks.BigContents()
        for name, contents in (
            (usb_disks.SMALL_FILE, usb_disks.SMALL_CONTENTS),
            ("hub.txt", HUB_NOTE),
            ("COPY.BIN", big),
        ):
            if Mtype(spec, name) != contents:
                return f"sdc1: mtools read different {name} contents"
        return None

    def PassMessage(self, result):
        return self.passed
