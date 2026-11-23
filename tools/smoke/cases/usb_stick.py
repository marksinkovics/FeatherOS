import os
import re
import shutil
import struct
import subprocess
from pathlib import Path

from create_usb_disks import SECTOR_SIZE

from smoke.boot import PANIC_MARKER
from smoke.cases.base import INPUT_TIMEOUT_SECONDS, Case, SkipCase
from smoke.disk_checks import (
    FatCopiesAgree,
    FindFsckFat,
    FsckFat,
    Mdir,
    MtoolsSpec,
    Mtype,
    PartitionBytes,
)
from smoke.script import Lines, WaitFor

ROOT_DIRECTORY = Path(__file__).resolve().parents[3]
LOG_FILE = "dmesg-1.txt"
LOG_MARKER = b"Limine transferred control"
SHELL_INPUT = f"""cat /proc/partitions
mkdir /mnt
mount -t fat16 sda2 /mnt
dmesg > /mnt/logs/{LOG_FILE}
ls /mnt/logs
eject sda
echo stick saved
""".encode()
PARTITION_LINES = (
    r"^sda +\d+ 512 usb removable rw\r?$",
    r"^sda2 +32768 512 fat16\r?$",
)
SHELL_OUTPUT = (f"{LOG_FILE} ", "stick saved")


def GptPartitions(image):
    """(first, end) sectors of GPT entries 1 and 2."""
    with open(image, "rb") as disk:
        disk.seek(2 * SECTOR_SIZE)
        entries = disk.read(2 * 128)
    return [
        (
            struct.unpack_from("<Q", entries, index * 128 + 32)[0],
            struct.unpack_from("<Q", entries, index * 128 + 40)[0] + 1,
        )
        for index in range(2)
    ]


class UsbStick(Case):
    """The image tools/make_usb_stick.sh builds (a FAT32 ESP with Limine and
    the kernel, then a FAT16 log partition) boots under OVMF from QEMU's
    usb-storage drive with no virtio disk. The shell mounts the log
    partition as sda2, saves dmesg to it and ejects the stick. The harness
    then reads the file with mtools, runs fsck.fat -n on the log partition
    and checks that the ESP is byte for byte what the script wrote. The
    harness works on a copy of the image made for each run."""

    timeout = INPUT_TIMEOUT_SECONDS
    serial_input = True
    shuts_down = False
    has_disk = False
    virtio_keyboard = False
    passed = (
        "booted from the stick, saved dmesg to its FAT16 log partition, "
        "ejected it, and the host found the log and an unchanged ESP"
    )

    def __init__(self, args):
        super().__init__(args)
        self.image = Path(args.iso).parent / "usb-stick-run.img"
        self.pristine = Path(args.iso).parent / "usb-stick-pristine.img"
        self.fsck_note = ""

    def BootMedia(self):
        return self.image

    def Start(self):
        if shutil.which("mformat") is None:
            raise SkipCase("mtools not found; install mtools")
        build = subprocess.run(
            [
                "bash",
                str(ROOT_DIRECTORY / "tools" / "make_usb_stick.sh"),
                str(self.pristine),
            ],
            env={
                "PATH": os.environ["PATH"],
                "BUILD_DIR": str(self.args.build_dir),
                "LIMINE_TIMEOUT": "1",
            },
            capture_output=True,
            text=True,
            check=False,
        )
        if build.returncode != 0:
            raise RuntimeError(f"make_usb_stick.sh failed: {build.stderr}")
        shutil.copyfile(self.pristine, self.image)

    def QemuArguments(self):
        return [
            "-device",
            "qemu-xhci,id=xhci",
            "-drive",
            f"if=none,id=stick,file={self.image},format=raw",
            "-device",
            "usb-storage,bus=xhci.0,drive=stick,removable=on,bootindex=0",
        ]

    def Script(self, session):
        return [WaitFor("Storage: sda2: fat16"), *Lines(SHELL_INPUT)]

    def Markers(self):
        return (PANIC_MARKER.encode(), b"\nstick saved")

    def Check(self, result):
        output = result.output
        if PANIC_MARKER in output:
            return f"the kernel panicked{result.limit_note}"
        for pattern in PARTITION_LINES:
            if re.search(pattern, output, re.MULTILINE) is None:
                return f"/proc/partitions lacks a line matching {pattern}"
        for text in (
            "Storage: sda1: no supported filesystem",
            "Storage: sda2: fat16",
            "Storage: no disk",
        ):
            if text not in output:
                return f"the log lacks '{text}'{result.limit_note}"
        for line in SHELL_OUTPUT:
            if re.search(rf"^{re.escape(line)}", output, re.MULTILINE) is None:
                return (
                    f"the shell session never printed '{line}'"
                    f"{result.limit_note}"
                )
        if "ejected" not in output:
            return "eject did not log the ejection"
        failure = self.CheckImage()
        if failure is None:
            self.image.unlink(missing_ok=True)
            self.pristine.unlink(missing_ok=True)
        return failure

    def CheckImage(self):
        (esp_first, esp_end), (log_first, log_end) = GptPartitions(self.image)
        log_spec = MtoolsSpec(self.image, log_first)
        saved = Mtype(log_spec, f"logs/{LOG_FILE}")
        if saved is None:
            return f"mtools cannot read logs/{LOG_FILE} on the log partition"
        if LOG_MARKER not in saved:
            return f"logs/{LOG_FILE} lacks '{LOG_MARKER.decode()}'"
        if not FatCopiesAgree(self.image, log_first):
            return "the log partition's FAT copies differ"
        failure = FsckFat(self.image, log_first, log_end)
        if failure is not None:
            return failure
        if FindFsckFat() is None:
            self.fsck_note = " (fsck.fat not installed: not checked)"
        pristine_log = PartitionBytes(self.pristine, log_first, log_end)
        if PartitionBytes(self.image, log_first, log_end) == pristine_log:
            return "the log partition is unchanged: nothing was saved"
        esp_spec = MtoolsSpec(self.image, esp_first)
        pristine_spec = MtoolsSpec(self.pristine, esp_first)
        if Mdir(esp_spec) != Mdir(pristine_spec) or Mdir(esp_spec) is None:
            return "the ESP's directory listing changed"
        for name, source in (
            ("kernel.elf", "kernel.elf"),
            ("initramfs.tar", "initramfs.tar"),
        ):
            built = (Path(self.args.build_dir) / source).read_bytes()
            if Mtype(esp_spec, name) != built:
                return f"the ESP's {name} is not the built file"
        if PartitionBytes(self.image, esp_first, esp_end) != PartitionBytes(
            self.pristine, esp_first, esp_end
        ):
            return "the ESP's bytes changed"
        return None

    def PassMessage(self, result):
        return self.passed + self.fsck_note
