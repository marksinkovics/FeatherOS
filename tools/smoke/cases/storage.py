import os
import re
import struct
import subprocess

from smoke.boot import PANIC_MARKER
from smoke.cases.base import SHELL_TIMEOUT_SECONDS, Case
from smoke.script import Lines

STORAGE_MESSAGE = (
    "KERN/STORAGE: sector and FAT16 file writes verified; boot count "
)
PERSISTENT_FILE_LINES = 200
# Shell commands for each STORAGE_TEST boot: the first writes notes.txt, and
# the second must still find it after the restart.
STORAGE_SHELL_INPUT = {
    1: b"write notes.txt first line\nwrite -a notes.txt second line\n"
    b"cat notes.txt\nmkdir docs\nwrite docs/plan.txt nested hello\n"
    b'cat "/read me first.txt"\nwrite "My Plans.txt" long name test\n'
    b"ls docs\nls\n",
    2: b"cat notes.txt\nrm notes.txt\ncat notes.txt\nrm notes.txt\n"
    b"cat docs/plan.txt\nrmdir docs\nrm docs/plan.txt\nrmdir docs\n"
    b'cat "my plans.txt"\nrm "MY PLANS.TXT"\nls docs\nls\n',
}
NOTES_CONTENTS = b"first line\nsecond line\n"
PLAN_CONTENTS = b"nested hello\n"
LONG_NAME_CONTENTS = b"long name test\n"
NOTES_OUTPUT = re.compile(r"^first line\r?\nsecond line\r?$", re.MULTILINE)
# The shell runs in /home; the disk's root also holds BLOCK.TXT, the kernel
# test's PERSIST.TXT, and Read Me First.txt, written by mtools.
STORAGE_SHELL_OUTPUT = {
    1: (
        "\nLong names from mtools work.",
        "plan.txt",
        "notes.txt",
        "My Plans.txt",
        "docs/",
        "1 directory, 2 files, ",
    ),
    2: (
        "cat: cannot open notes.txt",
        "rm: notes.txt: no such file",
        "\nnested hello",
        "rmdir: docs: not an empty directory",
        "\nlong name test",
        "ls: docs: no such directory",
        "0 files, ",
    ),
}


def PartitionOffset(disk):
    """Byte offset of the first partition, from a GPT or the MBR."""
    mbr = disk.read(512)
    if mbr[446 + 4] != 0xEE:
        return struct.unpack_from("<I", mbr, 446 + 8)[0] * 512
    disk.seek(512)
    header = disk.read(512)
    disk.seek(struct.unpack_from("<Q", header, 72)[0] * 512)
    return struct.unpack_from("<Q", disk.read(128), 32)[0] * 512


def VerifyDiskImage(disk_path, boot_count):
    """Checks the guest's FAT16 writes with mtools, a separate FAT reader."""
    with open(disk_path, "rb") as disk:
        partition_offset = PartitionOffset(disk)
        disk.seek(partition_offset)
        boot_sector = disk.read(512)
        reserved_sectors = struct.unpack_from("<H", boot_sector, 14)[0]
        fat_count = boot_sector[16]
        fat_bytes = struct.unpack_from("<H", boot_sector, 22)[0] * 512
        disk.seek(partition_offset + reserved_sectors * 512)
        fats = [disk.read(fat_bytes) for _ in range(fat_count)]
    if any(fat != fats[0] for fat in fats):
        return "the FAT copies differ"

    image = f"{disk_path}@@{partition_offset}"
    environment = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    expected = "".join(
        f"FeatherOS boot {boot_count} line {line}\n"
        for line in range(PERSISTENT_FILE_LINES)
    ).encode()
    contents = subprocess.run(
        ["mtype", "-i", image, "::PERSIST.TXT"],
        capture_output=True,
        env=environment,
        check=False,
    )
    if contents.returncode != 0 or contents.stdout != expected:
        return "mtools read different PERSIST.TXT contents"
    listing = subprocess.run(
        ["mdir", "-b", "-i", image, "::"],
        capture_output=True,
        text=True,
        env=environment,
        check=False,
    )
    if listing.returncode != 0 or "SCRATCH" in listing.stdout:
        return "mtools still lists the removed scratch file"
    notes = subprocess.run(
        ["mtype", "-i", image, "::home/notes.txt"],
        capture_output=True,
        env=environment,
        check=False,
    )
    plan = subprocess.run(
        ["mtype", "-i", image, "::home/docs/plan.txt"],
        capture_output=True,
        env=environment,
        check=False,
    )
    if boot_count == 1 and notes.stdout != NOTES_CONTENTS:
        return "mtools read different home/notes.txt contents"
    long_name = subprocess.run(
        ["mtype", "-i", image, "::home/My Plans.txt"],
        capture_output=True,
        env=environment,
        check=False,
    )
    if boot_count == 1 and plan.stdout != PLAN_CONTENTS:
        return "mtools read different home/docs/plan.txt contents"
    if boot_count == 1 and long_name.stdout != LONG_NAME_CONTENTS:
        return "mtools read different 'My Plans.txt' contents"
    home_listing = subprocess.run(
        ["mdir", "-b", "-i", image, "::home"],
        capture_output=True,
        text=True,
        env=environment,
        check=False,
    )
    listed = home_listing.stdout.upper()
    if boot_count == 2 and any(
        name in listed for name in ("NOTES", "DOCS", "MY PLANS")
    ):
        return "mtools still lists a file or directory the shell removed"
    return None


class Storage(Case):
    timeout = SHELL_TIMEOUT_SECONDS
    serial_input = True

    def __init__(self, args):
        super().__init__(args)
        self.boot = args.expect_storage_boot

    def Script(self, session):
        return Lines(STORAGE_SHELL_INPUT[self.boot])

    def Markers(self):
        return (
            STORAGE_SHELL_OUTPUT[self.boot][-1].encode(),
            PANIC_MARKER.encode(),
        )

    def Check(self, result):
        output = result.output
        expected = f"{STORAGE_MESSAGE}{self.boot}"
        if (
            re.search(rf"^{re.escape(expected)}\r?$", output, re.MULTILINE)
            is None
        ):
            return f"expected '{expected}'"
        missing = [
            text
            for text in STORAGE_SHELL_OUTPUT[self.boot]
            if text not in output
        ]
        if missing or NOTES_OUTPUT.search(output) is None:
            return (
                "file tools output is missing "
                f"{missing or 'the cat output'}{result.limit_note}"
            )
        disk_error = VerifyDiskImage(self.args.disk, self.boot)
        if disk_error is not None:
            return f"disk image check: {disk_error}"
        return None

    def PassMessage(self, result):
        return (
            "sector, FAT16, and shell file-tool writes verified in the "
            f"guest and with mtools; boot {self.boot} persisted"
        )
