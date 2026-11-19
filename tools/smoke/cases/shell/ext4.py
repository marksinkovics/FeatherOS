import re

from smoke.cases.shell.part import Part
from smoke.script import Lines

# The ext4 (vda4) and ext3 (vda3) partitions (tools/create_test_disk.py): -r
# keeps ext4 read-only, a plain mount takes writes, and the changes are undone
# but for /shell.txt, which the shell case's checks (VerifyJournalledClean)
# find on both partitions after the unmounts closed their journals.
EXT4_INPUT = (
    b"mount -t ext2 vda4 /mnt\nmount -r -t ext4 vda4 /mnt\n"
    b"grep /mnt /proc/mounts\nwrite /mnt/new.txt x\numount /mnt\n"
    b"mount -t ext4 vda4 /mnt\ngrep /mnt /proc/mounts\nls -l /mnt\n"
    b"cat /mnt/hello.txt\ncat /mnt/link\nreadlink /mnt/link\n"
    b"wc -c /mnt/extents.bin\nmkdir /mnt/made\n"
    b"write /mnt/made/new.txt shell wrote this\n"
    b"mv /mnt/made/new.txt /mnt/made/renamed.txt\n"
    b"ln /mnt/made/renamed.txt /mnt/hard.txt\n"
    b"ln -s made/renamed.txt /mnt/soft\ncat /mnt/soft\nls -l /mnt/made\n"
    b"rmdir /mnt/made\ncp /mnt/extents.bin /mnt/copy.bin\n"
    b"wc -c /mnt/copy.bin\n"
    b"rm /mnt/copy.bin\nrm /mnt/soft\nrm /mnt/hard.txt\n"
    b"rm /mnt/made/renamed.txt\nrmdir /mnt/made\n"
    b"write /mnt/shell.txt shell wrote ext4\numount /mnt\n"
    b"mount -t ext3 vda3 /mnt\ngrep /mnt /proc/mounts\ncat /mnt/link\n"
    b"wc -c /mnt/big.txt\nwrite /mnt/shell.txt shell wrote ext3\n"
    b"mkdir /mnt/d\nrmdir /mnt/d\numount /mnt\n"
)
EXPECTED_EXT4_OUTPUT = (
    "mount: vda4 on /mnt: No such file or directory",
    "vda4 /mnt ext4 ro 0 0",
    "write: cannot open /mnt/new.txt: Read-only file system",
    "vda4 /mnt ext4 rw 0 0",
    "\nHello from the ext4 partition.",
    "\nnested ext4 file",
    "2097152 /mnt/extents.bin",
    "\nshell wrote this",
    "rmdir: /mnt/made: Directory not empty",
    "2097152 /mnt/copy.bin",
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
        r"> readlink /mnt/link\r?\ndocs/nested/deep\.txt\r?$",
        r"^-rw-rw-rw- +2 +0 +0 +17 \S+ \S+ renamed\.txt\r?$",
    )
)


def Check(result):
    for line in LINES:
        if line.search(result.output) is None:
            return f"the ext4 partition did not show {line.pattern!r}"
    return None


PART = Part(Lines(EXT4_INPUT), EXPECTED_EXT4_OUTPUT, Check)
