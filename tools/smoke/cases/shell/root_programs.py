import re

from smoke.cases.shell.part import Part
from smoke.script import Lines

# The root's /sbin holds init and the administrative programs and /bin the
# rest (build.sh, tools/create_test_disk.py), and PATH lists /bin, /sbin and
# /system in that order. A program copied over /bin/echo or /sbin/mount runs
# ahead of /system's, deleting it falls back to /system, and copying it back
# restores the root.
ADMINISTRATIVE_PROGRAMS = (
    "dhcp",
    "eject",
    "init",
    "mount",
    "net",
    "sync",
    "umount",
)
ROOT_PROGRAMS_INPUT = (
    b"ls /sbin\nls /bin\n"
    b"cp /system/date /bin/echo\necho\n"
    b"rm /bin/echo\necho back in system\n"
    b"cp /system/echo /bin/echo\n/bin/echo from bin\necho and PATH\n"
    b"cp /system/date /sbin/mount\nmount\n"
    b"rm /sbin/mount\nmount\n"
    b"cp /system/mount /sbin/mount\n/sbin/mount\nls /sbin\n"
)
EXPECTED_ROOT_PROGRAMS_OUTPUT = (
    "\ninit",
    "\nfullhello ",
    "\nback in system",
    "\nfrom bin",
    "\nand PATH",
)
DATE_LINE = r"\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} UTC\r?$"
BIN_FIRST = re.compile(r"> echo\r?\n" + DATE_LINE, re.MULTILINE)
SBIN_FIRST = re.compile(r"> mount\r?\n" + DATE_LINE, re.MULTILINE)
SYSTEM_FALLBACK = re.compile(r"> mount\r?\ninitramfs /system initramfs ro 0 0")
SBIN_RESTORED = re.compile(
    r"> /sbin/mount\r?\ninitramfs /system initramfs ro 0 0"
)
LISTING = re.compile(r"> ls /(bin|sbin)\r?\n(.*?)\r?\n(?=/home> )", re.DOTALL)


def Listed(listing, name):
    return re.search(rf"^{name} +\d+\r?$", listing, re.MULTILINE) is not None


def Check(result):
    if BIN_FIRST.search(result.output) is None:
        return "a program in /bin did not run ahead of /system's"
    if SBIN_FIRST.search(result.output) is None:
        return "a program in /sbin did not run ahead of /system's"
    if SYSTEM_FALLBACK.search(result.output) is None:
        return "deleting /sbin/mount did not fall back to /system/mount"
    if SBIN_RESTORED.search(result.output) is None:
        return "the restored /sbin/mount did not run"
    listings = {}
    for match in LISTING.finditer(result.output):
        listings.setdefault(match.group(1), []).append(match.group(2))
    if len(listings.get("sbin", [])) != 2 or "bin" not in listings:
        return "ls /sbin and ls /bin did not list"
    for sbin in listings["sbin"]:
        for name in ADMINISTRATIVE_PROGRAMS:
            if not Listed(sbin, name):
                return f"{name} is not in /sbin"
    for name in ADMINISTRATIVE_PROGRAMS:
        if Listed(listings["bin"][0], name):
            return f"{name} is in /bin, which holds only the other programs"
    return None


PART = Part(Lines(ROOT_PROGRAMS_INPUT), EXPECTED_ROOT_PROGRAMS_OUTPUT, Check)
