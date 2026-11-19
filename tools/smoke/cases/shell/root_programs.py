import re

from smoke.cases.shell.part import Part
from smoke.script import Lines

# The root's /bin holds the programs and /sbin only init
# (tools/create_test_disk.py), and PATH lists /bin first. A program copied
# over /bin/echo runs ahead of /system's, deleting it falls back to /system,
# and copying echo back restores the root.
ROOT_PROGRAMS_INPUT = (
    b"ls /sbin\nls /bin\n"
    b"cp /system/date /bin/echo\necho\n"
    b"rm /bin/echo\necho back in system\n"
    b"cp /system/echo /bin/echo\n/bin/echo from bin\necho and PATH\n"
)
EXPECTED_ROOT_PROGRAMS_OUTPUT = (
    "\ninit",
    "\nfullhello ",
    "\nback in system",
    "\nfrom bin",
    "\nand PATH",
)
BIN_FIRST = re.compile(
    r"> echo\r?\n\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} UTC\r?$", re.MULTILINE
)
BIN_LISTING = re.compile(r"> ls /bin\r?\n(.*?)\r?\n/home> ", re.DOTALL)
INIT_LISTED = re.compile(r"^init +\d+\r?$", re.MULTILINE)


def Check(result):
    if BIN_FIRST.search(result.output) is None:
        return "a program in /bin did not run ahead of /system's"
    listing = BIN_LISTING.search(result.output)
    if listing is None or INIT_LISTED.search(listing.group(1)) is not None:
        return "init is in /bin, which holds only /sbin's other programs"
    return None


PART = Part(Lines(ROOT_PROGRAMS_INPUT), EXPECTED_ROOT_PROGRAMS_OUTPUT, Check)
