import re

from create_storage_disks import SECTOR_SIZE

from smoke.cases.nvme import (
    AHCI_FAILURES,
    DISK_KIBIBYTES,
    KERNEL_MESSAGES,
    Naming,
    Nvme,
)

AHCI = Naming(
    "sda", "", "AHCI", "ahci", "sda1 to sda4 mounted, read and unmounted"
)
LOCATION = r"[0-9a-f]{2}:[0-9a-f]{2}\.0"
SERIAL = "feather-sata0"
# ich9-ahci has six ports, 32 command slots and 64-bit addressing.
CONTROLLER_LINE = re.compile(
    rf"KERN/I: Storage: ahci0: {LOCATION} 6 ports, 32 command slots, "
    r"(MSI-X|MSI|polling), 64-bit DMA\r?$",
    re.MULTILINE,
)
POLICY_LINE = "KERN/I: Storage: ahci0: QEMU controller, writes allowed"
DISK_LINE = re.compile(
    rf"KERN/I: Storage: sda: AHCI {LOCATION} port 0, \"QEMU HARDDISK\" "
    rf"\"{SERIAL}\", {DISK_KIBIBYTES} KiB, {SECTOR_SIZE}-byte sectors, "
    r"write cache, rw\r?$",
    re.MULTILINE,
)
ATAPI_LINE = re.compile(
    rf"KERN/W: Storage: AHCI {LOCATION} port 2: ATAPI device not supported"
    r"\r?$",
    re.MULTILINE,
)
KERNEL_MESSAGES_AHCI = ("KERN/I: Storage: sda: parked",) + KERNEL_MESSAGES[1:]


def QemuArguments(image):
    return [
        "-drive",
        f"if=none,id=sata_disk,file={image},format=raw",
        "-device",
        "ich9-ahci,id=ahci",
        "-device",
        f"ide-hd,drive=sata_disk,bus=ahci.0,serial={SERIAL}",
        # An empty ATAPI drive on port 2, which the driver must refuse.
        "-device",
        "ide-cd,bus=ahci.2",
    ]


class Ahci(Nvme):
    """An emulated ich9-ahci controller with an ide-hd on port 0 over a
    scratch copy of storage-controller.img and an ide-cd on port 2. The kernel
    probes the HBA on the storage worker, refuses the ATAPI device with a log
    line, registers sda and opens the four volumes. The checks are those of the
    NVME_TEST on sda1 to sda4, and shutdown must park the disk with STANDBY
    IMMEDIATE."""

    naming = AHCI
    controller_line = CONTROLLER_LINE
    policy_line = POLICY_LINE
    disk_line = DISK_LINE
    kernel_messages = KERNEL_MESSAGES_AHCI
    failures = AHCI_FAILURES
    what = "AHCI"
    passed = (
        "the AHCI disk was probed, four volumes were written and read back, "
        "and the image passed the host checks"
    )

    def QemuArguments(self):
        return QemuArguments(self.image)

    def CheckBoot(self, output):
        failure = super().CheckBoot(output)
        if failure is None and ATAPI_LINE.search(output) is None:
            return "the log lacks the refusal of the ATAPI device on port 2"
        return failure
