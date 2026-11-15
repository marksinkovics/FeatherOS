import re

from smoke.cases.shell.part import Part
from smoke.script import Lines

# tools/create_test_disk.py puts a read-only ext2 volume in the disk's second
# partition, vda2, which nothing mounts at boot.
EXT2_INPUT = (
    b"mount -t ext2 vda2 /mnt\ngrep ext2 /proc/mounts\nls /mnt\n"
    b"cat /mnt/hello.txt\ncat /mnt/docs/notes/deep.txt\nwc -c /mnt/big.txt\n"
    b"write /mnt/new.txt x\nrm /mnt/hello.txt\ncat /mnt/fast-link\n"
    b"umount /mnt\nls /mnt\n"
)
EXPECTED_EXT2_OUTPUT = (
    "vda2 /mnt ext2 ro 0 0",
    "lost+found/",
    "\nHello from the ext2 partition.",
    "\nnested ext2 file",
    "80000 /mnt/big.txt",
    "write: cannot open /mnt/new.txt: Read-only file system",
    "rm: /mnt/hello.txt: read-only file system",
    "cat: cannot open /mnt/fast-link",
    "ls: /mnt: no such directory",
)
# ls shows the first 63 bytes of a longer name, and links as files that
# stat cannot open, so they and the long name count 0 bytes.
LISTING = re.compile(
    r"^a-file-name-longer-than-sixty-three-characters-that-fat16-canno 0\r?$"
    r"(?s:.*?)^2 directories, 5 files, 80031 bytes\r?$",
    re.MULTILINE,
)


def Check(result):
    if LISTING.search(result.output) is None:
        return "ls /mnt did not list the ext2 volume's files"
    return None


PART = Part(Lines(EXT2_INPUT), EXPECTED_EXT2_OUTPUT, Check)
