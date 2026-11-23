import os
import re
import tempfile

from smoke.cases.base import KernelTest

IMAGE_MEBIBYTES = 4
SECTOR_SIZE = 512
DEVICE_LINE = re.compile(
    r"KERN/I: USB 1-(\d+): super-speed 46f4:0001 \"QEMU USB HARDDRIVE\" "
    r"usb-storage"
)
STORAGE_LINE = re.compile(
    r"KERN/I: Storage: USB 1-(\d+), \"QEMU\" \"QEMU HARDDISK\", "
    rf"{IMAGE_MEBIBYTES * 1024} KiB, {SECTOR_SIZE}-byte sectors, "
    r"removable\r?$",
    re.MULTILINE,
)
SECTORS_LINE = (
    "KERN/USBSTORAGE: "
    f"{IMAGE_MEBIBYTES * 1024 * 1024 // SECTOR_SIZE} sectors of "
    f"{SECTOR_SIZE} bytes"
)
STEPS = (
    "KERN/USBSTORAGE: last sector written and read back",
    "KERN/USBSTORAGE: transfer past the end refused",
    "KERN/USBSTORAGE: multi-command transfer matched",
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
    image: the kernel enumerates it at SuperSpeed, binds usb-storage, probes
    it through INQUIRY, TEST UNIT READY, READ CAPACITY and MODE SENSE, and
    its test hook writes and reads a pattern at the last sector, checks that
    transfers past the end are refused, and writes and reads back 128 KiB
    that takes several commands."""

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
        with open(self.image, "wb") as image:
            image.truncate(IMAGE_MEBIBYTES * 1024 * 1024)

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
