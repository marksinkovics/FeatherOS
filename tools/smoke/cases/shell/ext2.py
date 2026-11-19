import re

from smoke.cases.shell.part import Part
from smoke.script import Lines

# The ext2 partition is vda4 (tools/create_test_disk.py). Everything on it but
# lost+found belongs to user and group 1000, hello.txt is 0640 in group 100,
# and every time is 2026-01-01. The part first mounts the FAT16 partition,
# vda2, read-only at /mnt, then mounts ext2 there (the wrong type is refused;
# read-only refuses changes) and writes on it, leaving it as it was; the
# unmount marks it clean, and the shell case has e2fsck check it.
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
    b"mount -t ext4 vda4 /mnt\nmount -r -t ext2 vda4 /mnt\n"
    b"grep /mnt /proc/mounts\nwrite /mnt/new2.txt x\numount /mnt\n"
    b"mount -t ext2 vda4 /mnt\ngrep /mnt /proc/mounts\n"
    b"ls /mnt\nls -l /mnt\n"
    b"cat /mnt/" + LONG_EXT2_NAME + b"\n"
    b"cat /mnt/hello.txt\ncat /mnt/docs/notes/deep.txt\n"
    b"wc -c /mnt/big.txt\n"
    b"cat /mnt/fast-link\ncat /mnt/slow-link\n"
    b"mkdir /mnt/made\nwrite /mnt/made/new.txt shell wrote this\n"
    b"mv /mnt/made/new.txt /mnt/made/renamed.txt\n"
    b"ln /mnt/made/renamed.txt /mnt/hard.txt\n"
    b"ln -s made/renamed.txt /mnt/soft\n"
    b"cat /mnt/soft\nls -l /mnt/made\n"
    b"rmdir /mnt/made/.\nrmdir /mnt/made\n"
    b"mv /mnt/made/.. /mnt/up\n"
    b"rm /mnt/soft\nrm /mnt/hard.txt\nrm /mnt/made/renamed.txt\n"
    b"rmdir /mnt/made\n"
    b"umount /mnt\n"
    b"write /tmp/" + LONG_TMP_NAME + b" kept\nls /tmp\n"
)
EXPECTED_EXT2_OUTPUT = (
    "vda2 /mnt fat16 ro 0 0",
    "\nLong names from mtools work.",
    "write: cannot open /mnt/new.txt: Read-only file system",
    "rm: /mnt/Read Me First.txt: read-only file system",
    "ls: /mnt: no such directory",
    "mount: vda4 on /mnt: No such file or directory",
    "vda4 /mnt ext2 ro 0 0",
    "write: cannot open /mnt/new2.txt: Read-only file system",
    "vda4 /mnt ext2 rw 0 0",
    "\nlong ext2 name",
    "\nHello from the ext2 partition.",
    "\nnested ext2 file",
    "80000 /mnt/big.txt",
    "rmdir: /mnt/made/.: Invalid argument",
    "rmdir: /mnt/made: Directory not empty",
    "mv: /mnt/made/..: Device or resource busy",
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
        r"> cat /mnt/fast-link\r?\nHello from the ext2 partition\.\r?$",
        r"> cat /mnt/slow-link\r?\nnested ext2 file\r?$",
    )
)
# The hard link counts twice, and the link leads to the renamed file.
WRITTEN = tuple(
    re.compile(line, re.MULTILINE)
    for line in (
        r"> cat /mnt/soft\r?\nshell wrote this\r?$",
        r"^-rw-rw-rw- +2 +0 +0 +17 \S+ \S+ renamed\.txt\r?$",
    )
)
TMP_LISTING = re.compile(
    r"^" + re.escape(LONG_TMP_NAME.decode()) + r" 5\r?$", re.MULTILINE
)


def Check(result):
    if LISTING.search(result.output) is None:
        return "ls /mnt did not list the ext2 partition's files"
    for line in LONG_LISTING:
        if line.search(result.output) is None:
            return f"ls -l /mnt is missing a line like {line.pattern!r}"
    for line in THROUGH_LINKS:
        if line.search(result.output) is None:
            return f"cat through an ext2 link did not print {line.pattern!r}"
    for line in WRITTEN:
        if line.search(result.output) is None:
            return f"the ext2 partition did not show {line.pattern!r}"
    if TMP_LISTING.search(result.output) is None:
        return "ls /tmp did not list the long tmpfs name whole"
    return None


PART = Part(Lines(EXT2_INPUT), EXPECTED_EXT2_OUTPUT, Check)
