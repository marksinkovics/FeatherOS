import re

from smoke.cases.shell.part import Part
from smoke.script import Lines

# The root is ext2 (tools/create_test_disk.py). Everything in /samples belongs
# to user and group 1000, hello.txt is 0640 in group 100, and every time is
# 2026-01-01. The part first mounts the FAT16 partition, vda2, read-only at
# /mnt, then writes in /samples and leaves it as it was; the shell case syncs
# and then has e2fsck check the root.
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
    b"mount -r -t fat16 vda2 /mnt\ngrep /mnt /proc/mounts\n"
    b'cat "/mnt/read me first.txt"\nwrite /mnt/new.txt x\n'
    b'rm "/mnt/Read Me First.txt"\numount /mnt\nls /mnt\n'
    b"ls /samples\nls -l /samples\n"
    b"cat /samples/" + LONG_EXT2_NAME + b"\n"
    b"cat /samples/hello.txt\ncat /samples/docs/notes/deep.txt\n"
    b"wc -c /samples/big.txt\n"
    b"cat /samples/fast-link\ncat /samples/slow-link\n"
    b"mkdir /samples/made\nwrite /samples/made/new.txt shell wrote this\n"
    b"mv /samples/made/new.txt /samples/made/renamed.txt\n"
    b"ln /samples/made/renamed.txt /samples/hard.txt\n"
    b"ln -s made/renamed.txt /samples/soft\n"
    b"cat /samples/soft\nls -l /samples/made\n"
    b"rmdir /samples/made/.\nrmdir /samples/made\n"
    b"mv /samples/made/.. /samples/up\n"
    b"rm /samples/soft\nrm /samples/hard.txt\nrm /samples/made/renamed.txt\n"
    b"rmdir /samples/made\n"
    b"write /tmp/" + LONG_TMP_NAME + b" kept\nls /tmp\n"
)
EXPECTED_EXT2_OUTPUT = (
    "vda2 /mnt fat16 ro 0 0",
    "\nLong names from mtools work.",
    "write: cannot open /mnt/new.txt: Read-only file system",
    "rm: /mnt/Read Me First.txt: read-only file system",
    "ls: /mnt: no such directory",
    "\nlong ext2 name",
    "\nHello from the ext2 partition.",
    "\nnested ext2 file",
    "80000 /samples/big.txt",
    "rmdir: /samples/made/.: Invalid argument",
    "rmdir: /samples/made: Directory not empty",
    "mv: /samples/made/..: Device or resource busy",
)
# ls shows the long name whole, and stat gives links their own size.
LISTING = re.compile(
    r"^" + re.escape(LONG_EXT2_NAME.decode()) + r" 15\r?$"
    r"(?s:.*?)^1 directory, 5 files, 80119 bytes\r?$",
    re.MULTILINE,
)
LONG_LISTING = tuple(
    re.compile(line, re.MULTILINE)
    for line in (
        r"^-rw-r--r-- +1 +1000 +1000 +15 2026-01-01 00:00 "
        + re.escape(LONG_EXT2_NAME.decode())
        + r"\r?$",
        r"^drwxr-xr-x +3 +1000 +1000 +1024 2026-01-01 00:00 docs\r?$",
        r"^lrwxrwxrwx +1 +1000 +1000 +9 2026-01-01 00:00 fast-link"
        r" -> hello\.txt\r?$",
        r"^lrwxrwxrwx +1 +1000 +1000 +64 2026-01-01 00:00 slow-link -> "
        r"docs/notes/(\.\./notes/){5}deep\.txt\r?$",
        r"^-rw-r----- +1 +1000 +100 +31 2026-01-01 00:00 hello\.txt\r?$",
    )
)
# The VFS follows both links: the fast one's target is in its inode, the slow
# one's in a block, through "../" five times.
THROUGH_LINKS = tuple(
    re.compile(line, re.MULTILINE)
    for line in (
        r"> cat /samples/fast-link\r?\nHello from the ext2 partition\.\r?$",
        r"> cat /samples/slow-link\r?\nnested ext2 file\r?$",
    )
)
# The hard link counts twice, and the link leads to the renamed file.
WRITTEN = tuple(
    re.compile(line, re.MULTILINE)
    for line in (
        r"> cat /samples/soft\r?\nshell wrote this\r?$",
        r"^-rw-rw-rw- +2 +0 +0 +17 \S+ \S+ renamed\.txt\r?$",
    )
)
TMP_LISTING = re.compile(
    r"^" + re.escape(LONG_TMP_NAME.decode()) + r" 5\r?$", re.MULTILINE
)


def Check(result):
    if LISTING.search(result.output) is None:
        return "ls /samples did not list the ext2 root's files"
    for line in LONG_LISTING:
        if line.search(result.output) is None:
            return f"ls -l /samples is missing a line like {line.pattern!r}"
    for line in THROUGH_LINKS:
        if line.search(result.output) is None:
            return f"cat through an ext2 link did not print {line.pattern!r}"
    for line in WRITTEN:
        if line.search(result.output) is None:
            return f"the ext2 root did not show {line.pattern!r}"
    if TMP_LISTING.search(result.output) is None:
        return "ls /tmp did not list the long tmpfs name whole"
    return None


PART = Part(Lines(EXT2_INPUT), EXPECTED_EXT2_OUTPUT, Check)
