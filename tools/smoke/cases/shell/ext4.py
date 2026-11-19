import re

from smoke.cases.shell.part import Part
from smoke.script import Lines

# The root is ext4 and /samples holds its test files
# (tools/create_test_disk.py). Its one instance is the root, so no second ext4
# mounts and the root cannot be mounted read-only from the shell. The part
# writes in /samples and undoes it but for /shell.txt. ext3 (vda3) mounts
# read-write at /mnt, and the shell case's checks (VerifyJournalledClean) find
# /shell.txt on both: ext3's journal closed by umount, the root's committed by
# the interrupt part's sync.
EXT4_INPUT = (
    b"mount -t ext2 vda1 /mnt\n"
    b"ls -l /samples\n"
    b"cat /samples/hello.txt\ncat /samples/link\nreadlink /samples/link\n"
    b"wc -c /samples/extents.bin\nmkdir /samples/made\n"
    b"write /samples/made/new.txt shell wrote this\n"
    b"mv /samples/made/new.txt /samples/made/renamed.txt\n"
    b"ln /samples/made/renamed.txt /samples/hard.txt\n"
    b"ln -s made/renamed.txt /samples/soft\ncat /samples/soft\n"
    b"ls -l /samples/made\n"
    b"rmdir /samples/made\ncp /samples/extents.bin /samples/copy.bin\n"
    b"wc -c /samples/copy.bin\n"
    b"rm /samples/copy.bin\nrm /samples/soft\nrm /samples/hard.txt\n"
    b"rm /samples/made/renamed.txt\nrmdir /samples/made\n"
    b"write /shell.txt shell wrote ext4\n"
    b"mount -t ext3 vda3 /mnt\ngrep /mnt /proc/mounts\ncat /mnt/link\n"
    b"wc -c /mnt/big.txt\nwrite /mnt/shell.txt shell wrote ext3\n"
    b"mkdir /mnt/d\nrmdir /mnt/d\numount /mnt\n"
)
EXPECTED_EXT4_OUTPUT = (
    "mount: vda1 on /mnt: No such file or directory",
    "\nHello from the ext4 partition.",
    "\nnested ext4 file",
    "2097152 /samples/extents.bin",
    "\nshell wrote this",
    "rmdir: /samples/made: Directory not empty",
    "2097152 /samples/copy.bin",
    "vda3 /mnt ext3 rw 0 0",
    "\next3 subdirectory file",
    "48000 /mnt/big.txt",
)
LINES = tuple(
    re.compile(line, re.MULTILINE)
    for line in (
        r"^drwxr-xr-x +3 +0 +0 +1024 2026-01-01 00:00 docs\r?$",
        r"^-rw-r--r-- +1 +0 +0 +2097152 2026-01-01 00:00 extents\.bin\r?$",
        r"^lrwxrwxrwx +1 +0 +0 +20 2026-01-01 00:00 link"
        r" -> docs/nested/deep\.txt\r?$",
        r"> readlink /samples/link\r?\ndocs/nested/deep\.txt\r?$",
        r"^-rw-rw-rw- +2 +0 +0 +17 \S+ \S+ renamed\.txt\r?$",
    )
)


def Check(result):
    for line in LINES:
        if line.search(result.output) is None:
            return f"the ext4 root did not show {line.pattern!r}"
    return None


PART = Part(Lines(EXT4_INPUT), EXPECTED_EXT4_OUTPUT, Check)
