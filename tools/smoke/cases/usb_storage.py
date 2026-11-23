import os
import re
import tempfile

from smoke import usb_image
from smoke.cases.base import KernelTest

SECTOR_SIZE = usb_image.disks.SECTOR_SIZE
IMAGE_MEBIBYTES = usb_image.IMAGE_SECTORS * SECTOR_SIZE // (1024 * 1024)
PARTITION_KIBIBYTES = usb_image.PARTITION_SECTORS * SECTOR_SIZE // 1024
DEVICE_LINE = re.compile(
    r"KERN/I: USB 1-(\d+): super-speed 46f4:0001 \"QEMU USB HARDDRIVE\" "
    r"usb-storage"
)
STORAGE_LINE = re.compile(
    r"KERN/I: Storage: sda: USB 1-(\d+), \"QEMU\" \"QEMU HARDDISK\", "
    rf"{IMAGE_MEBIBYTES * 1024} KiB, {SECTOR_SIZE}-byte sectors, "
    r"removable\r?$",
    re.MULTILINE,
)
VOLUME_LINES = (
    re.compile(
        rf"KERN/I: Storage: sda1: fat16, {PARTITION_KIBIBYTES} KiB\r?$",
        re.MULTILINE,
    ),
    re.compile(
        rf"KERN/I: Storage: sda2: ext2, {PARTITION_KIBIBYTES} KiB\r?$",
        re.MULTILINE,
    ),
)
PARTITION_LINES = (
    re.compile(
        r"KERN/USBSTORAGE: partitions: sda +\d+ 512 usb removable rw\r?$",
        re.MULTILINE,
    ),
    re.compile(
        rf"KERN/USBSTORAGE: partitions: sda1 +{PARTITION_KIBIBYTES} 512 "
        r"fat16\r?$",
        re.MULTILINE,
    ),
    re.compile(
        rf"KERN/USBSTORAGE: partitions: sda2 +{PARTITION_KIBIBYTES} 512 "
        r"ext2\r?$",
        re.MULTILINE,
    ),
)
SECTORS_LINE = (
    "KERN/USBSTORAGE: sda "
    f"{IMAGE_MEBIBYTES * 1024 * 1024 // SECTOR_SIZE} sectors of "
    f"{SECTOR_SIZE} bytes"
)
STEPS = (
    "KERN/USBSTORAGE: sda1 mounted, read and unmounted",
    "KERN/USBSTORAGE: sda2 mounted, read and unmounted",
    "KERN/USBSTORAGE: last sector written and read back",
    "KERN/USBSTORAGE: transfer past the end refused",
    "KERN/USBSTORAGE: multi-command transfer matched",
    "KERN/USBSTORAGE: counters matched",
    "KERN/USBSTORAGE: tests passed",
)
FAILURES = (
    "reset recovery",
    "could not",
    "enumeration failed",
    "driver could not attach",
)


class UsbStorage(KernelTest):
    """QEMU's usb-storage on its xHCI controller, backed by a scratch raw
    image with an MBR, a FAT16 partition and an ext2 partition: the kernel
    enumerates it at SuperSpeed, binds usb-storage, probes it through
    INQUIRY, TEST UNIT READY, READ CAPACITY and MODE SENSE, and the storage
    worker opens its volumes. The test hook lists /proc/partitions, mounts,
    reads and unmounts sda1 and sda2, then writes and reads a pattern at the
    last sector, checks that
    transfers past the end are refused, and writes and reads back 128 KiB
    that takes several commands, all through the registry's disk sda, whose
    counters it checks."""

    timeout = 40
    expected = "KERN/USBSTORAGE: tests passed"
    ends_on_panic = True
    failure = "the USB storage tests did not report success"
    passed = (
        "the USB drive was probed, and its last sector and a multi-command "
        "transfer read back what was written"
    )

    def __init__(self, args):
        super().__init__(args)
        self.directory = None
        self.image = None

    def Start(self):
        self.directory = tempfile.TemporaryDirectory(prefix="fq-")
        self.image = os.path.join(self.directory.name, "usb.img")
        usb_image.BuildImage(self.image)

    def Stop(self):
        if self.directory is not None:
            self.directory.cleanup()

    def QemuArguments(self):
        return [
            "-device",
            "qemu-xhci,id=xhci",
            "-drive",
            f"if=none,id=usbdisk,file={self.image},format=raw",
            "-device",
            "usb-storage,bus=xhci.0,drive=usbdisk,removable=on",
        ]

    def Check(self, result):
        failure = super().Check(result)
        if failure is not None:
            return failure
        output = result.output
        if DEVICE_LINE.search(output) is None:
            return "the boot log has no line for the USB storage device"
        if STORAGE_LINE.search(output) is None:
            return "the probe did not log the drive's model and size"
        for pattern in VOLUME_LINES:
            if pattern.search(output) is None:
                return f"the worker did not log a volume: {pattern.pattern}"
        for pattern in PARTITION_LINES:
            if pattern.search(output) is None:
                return (
                    f"/proc/partitions lacks a line matching {pattern.pattern}"
                )
        if SECTORS_LINE not in output:
            return f"the test hook did not report '{SECTORS_LINE}'"
        position = 0
        for step in STEPS:
            found = output.find(step, position)
            if found < 0:
                return f"the test hook never reported '{step}' in order"
            position = found
        kernel_lines = [
            line for line in output.splitlines() if "KERN/" in line
        ]
        for warning in FAILURES:
            if any(warning in line for line in kernel_lines):
                return f"the log reports a USB storage failure: {warning}"
        return None
