import re

from smoke.cases.shell.part import Part
from smoke.script import Lines

# tools/create_test_disk.py puts a read-only ext2 volume in the disk's second
# partition, vda2, which nothing mounts at boot. Its files belong to user and
# group 1000, hello.txt is 0640 in group 100, and every time is 2026-01-01.
LONG_EXT2_NAME = (
    b"a-file-name-longer-than-sixty-three-characters"
    b"-that-fat16-cannot-hold.txt"
)
# Past the 63 characters names used to be cut to; /tmp is tmpfs.
LONG_TMP_NAME = (
    b"a-tmpfs-name-longer-than-sixty-three-characters"
    b"-that-tmpfs-used-to-cut-short.txt"
)
EXT2_INPUT = (
    b"mount -t ext2 vda2 /mnt\ngrep ext2 /proc/mounts\nls /mnt\nls -l /mnt\n"
    b"cat /mnt/" + LONG_EXT2_NAME + b"\n"
    b"cat /mnt/hello.txt\ncat /mnt/docs/notes/deep.txt\nwc -c /mnt/big.txt\n"
    b"write /mnt/new.txt x\nrm /mnt/hello.txt\ncat /mnt/fast-link\n"
    b"umount /mnt\nls /mnt\n"
    b"write /tmp/" + LONG_TMP_NAME + b" kept\nls /tmp\n"
)
EXPECTED_EXT2_OUTPUT = (
    "vda2 /mnt ext2 ro 0 0",
    "lost+found/",
    "\nlong ext2 name",
    "\nHello from the ext2 partition.",
    "\nnested ext2 file",
    "80000 /mnt/big.txt",
    "write: cannot open /mnt/new.txt: Read-only file system",
    "rm: /mnt/hello.txt: read-only file system",
    "cat: cannot open /mnt/fast-link",
    "ls: /mnt: no such directory",
)
# ls shows the long name whole, and stat gives links their own size.
LISTING = re.compile(
    r"^" + re.escape(LONG_EXT2_NAME.decode()) + r" 15\r?$"
    r"(?s:.*?)^2 directories, 5 files, 80119 bytes\r?$",
    re.MULTILINE,
)
LONG_LISTING = tuple(
    re.compile(line, re.MULTILINE)
    for line in (
        r"^drwx------ +2 +0 +0 +12288 2026-01-01 00:00 lost\+found\r?$",
        r"^-rw-r--r-- +1 +1000 +1000 +15 2026-01-01 00:00 "
        + re.escape(LONG_EXT2_NAME.decode())
        + r"\r?$",
        r"^drwxr-xr-x +3 +1000 +1000 +1024 2026-01-01 00:00 docs\r?$",
        r"^lrwxrwxrwx +1 +1000 +1000 +9 2026-01-01 00:00 fast-link\r?$",
        r"^-rw-r----- +1 +1000 +100 +31 2026-01-01 00:00 hello\.txt\r?$",
    )
)
TMP_LISTING = re.compile(
    r"^" + re.escape(LONG_TMP_NAME.decode()) + r" 5\r?$", re.MULTILINE
)


def Check(result):
    if LISTING.search(result.output) is None:
        return "ls /mnt did not list the ext2 volume's files"
    for line in LONG_LISTING:
        if line.search(result.output) is None:
            return f"ls -l /mnt is missing a line like {line.pattern!r}"
    if TMP_LISTING.search(result.output) is None:
        return "ls /tmp did not list the long tmpfs name whole"
    return None


PART = Part(Lines(EXT2_INPUT), EXPECTED_EXT2_OUTPUT, Check)
