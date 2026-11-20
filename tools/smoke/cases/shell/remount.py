import re

from smoke.cases.shell.part import Part
from smoke.script import Lines

# Runs right after mounts, before anything holds a file open for writing on
# the root. /tmp, the read-only /system and /dev, a plain directory and a
# missing path give their errors; the root then goes read-only and back, the
# way STORAGE's second boot checks from outside.
REMOUNT_INPUT = (
    b"mount -o remount,ro /tmp\nwrite /tmp/ro.txt x\n"
    b"mount -o remount,rw /tmp\nwrite /tmp/ro.txt ok\ncat /tmp/ro.txt\n"
    b"rm /tmp/ro.txt\n"
    b"mount -o remount,rw /system\nmount -o remount,ro /dev\n"
    b"mount -o remount,ro /home\nmount -o remount,ro /nonexistent\n"
    b"mount -o remount /\n"
    b"mount -o remount,ro /\nwrite /ro.txt x\ngrep vda1 /proc/mounts\n"
    b"write /tmp/still.txt writable\ncat /tmp/still.txt\nrm /tmp/still.txt\n"
    b"mount -o rw,remount /\nwrite /ro.txt rw again\ncat /ro.txt\n"
    b"rm /ro.txt\ngrep vda1 /proc/mounts\n"
)
EXPECTED_REMOUNT_OUTPUT = (
    "write: cannot open /tmp/ro.txt: Read-only file system",
    "mount: /system: Read-only file system",
    "mount: /dev: Invalid argument",
    "mount: /home: Invalid argument",
    "mount: /nonexistent: No such file or directory",
    "       mount -o remount,ro|remount,rw <target>",
    "write: cannot open /ro.txt: Read-only file system",
    "\nwritable",
)
ORDER = re.compile(
    r"^ok\r?$.*?^vda1 / ext4 ro 0 0\r?$.*?^rw again\r?$.*?"
    r"^vda1 / ext4 rw 0 0\r?$",
    re.MULTILINE | re.DOTALL,
)


def Check(result):
    if ORDER.search(result.output) is None:
        return (
            "the root did not show ro after remount,ro and rw after "
            "remount,rw, or /tmp took no write after remount,rw"
        )
    return None


PART = Part(Lines(REMOUNT_INPUT), EXPECTED_REMOUNT_OUTPUT, Check)
