import re

from smoke.cases.shell.part import Part
from smoke.script import Lines

# The ext4 (vda4) and ext3 (vda3) partitions (tools/create_test_disk.py),
# mounted without -r: the core refuses to write them, so both mount read-only
# and every change fails. The shell case checks afterwards that neither
# partition changed.
EXT4_INPUT = (
    b"mount -t ext2 vda4 /mnt\nmount -t ext4 vda4 /mnt\n"
    b"grep /mnt /proc/mounts\nls -l /mnt\n"
    b"cat /mnt/hello.txt\ncat /mnt/link\nreadlink /mnt/link\n"
    b"wc -c /mnt/extents.bin\nwrite /mnt/new.txt x\numount /mnt\n"
    b"mount -t ext3 vda3 /mnt\ngrep /mnt /proc/mounts\ncat /mnt/link\n"
    b"wc -c /mnt/big.txt\nrm /mnt/hello.txt\numount /mnt\n"
)
EXPECTED_EXT4_OUTPUT = (
    "mount: vda4 on /mnt: No such file or directory",
    "vda4 /mnt ext4 ro 0 0",
    "\nHello from the ext4 partition.",
    "\nnested ext4 file",
    "2097152 /mnt/extents.bin",
    "write: cannot open /mnt/new.txt: Read-only file system",
    "vda3 /mnt ext3 ro 0 0",
    "\next3 subdirectory file",
    "48000 /mnt/big.txt",
    "rm: /mnt/hello.txt: read-only file system",
)
LINES = tuple(
    re.compile(line, re.MULTILINE)
    for line in (
        r"^drwxr-xr-x +3 +0 +0 +1024 2026-01-01 00:00 docs\r?$",
        r"^-rw-r--r-- +1 +0 +0 +2097152 2026-01-01 00:00 extents\.bin\r?$",
        r"^lrwxrwxrwx +1 +0 +0 +20 2026-01-01 00:00 link"
        r" -> docs/nested/deep\.txt\r?$",
        r"> readlink /mnt/link\r?\ndocs/nested/deep\.txt\r?$",
    )
)


def Check(result):
    for line in LINES:
        if line.search(result.output) is None:
            return f"the ext4 partition did not show {line.pattern!r}"
    return None


PART = Part(Lines(EXT4_INPUT), EXPECTED_EXT4_OUTPUT, Check)
