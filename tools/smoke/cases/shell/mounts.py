import re

from smoke.cases.shell.part import Part
from smoke.script import Lines

MOUNTS_INPUT = (
    b"mount\nwrite /tmp/mnt.txt survives umount\numount /tmp\nmount\nls /tmp\n"
    b"mount -t tmpfs tmp /tmp\ncat /tmp/mnt.txt\nmount\numount /\n"
    b"umount /nonexistent\nmount -t nosuchfs x /mnt\n"
)
EXPECTED_MOUNTS_OUTPUT = (
    "initramfs /system initramfs ro 0 0",
    "procfs /proc procfs ro 0 0",
    "ls: /tmp: no such directory",
    "survives umount",
    "umount: /: target is busy",
    "umount: /nonexistent: No such file or directory",
    "mount: nosuchfs: unknown filesystem type",
)
# The disk's and /tmp's lines from the three listings, in order.
MOUNT_LINE = re.compile(
    r"^(vda1 / ext4 rw 0 0|tmp /tmp tmpfs rw 0 0)\r?$", re.MULTILINE
)


def Check(result):
    end = result.output.find(EXPECTED_MOUNTS_OUTPUT[-1])
    lines = MOUNT_LINE.findall(result.output[: end if end >= 0 else None])
    disk = "vda1 / ext4 rw 0 0"
    tmp = "tmp /tmp tmpfs rw 0 0"
    # Mount order: /tmp before the disk at boot, gone after umount, last once
    # mounted again.
    if lines != [tmp, disk, disk, disk, tmp]:
        return f"mount did not follow umount and mount of /tmp: {lines}"
    return None


PART = Part(Lines(MOUNTS_INPUT), EXPECTED_MOUNTS_OUTPUT, Check)
