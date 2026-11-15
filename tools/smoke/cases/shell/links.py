import re

from smoke.cases.shell.part import Part
from smoke.script import Lines

# Symbolic links in /tmp, which holds them, unlike the FAT16 disk; tmpfs holds
# no hard links. The VFS follows links and stops loops after 40.
LINKS_INPUT = (
    b"ln -h\nreadlink -h\n"
    b"write /tmp/target.txt linked text\nln -s target.txt /tmp/link.txt\n"
    b"cat /tmp/link.txt\nreadlink /tmp/link.txt\nreadlink -f /tmp/link.txt\n"
    b"readlink /tmp/target.txt\nls -l /tmp\n"
    b"ln -s loop-b /tmp/loop-a\nln -s loop-a /tmp/loop-b\ncat /tmp/loop-a\n"
    b"ln /tmp/target.txt /tmp/hard.txt\n"
    b"rm /tmp/link.txt\ncat /tmp/target.txt\ncat /tmp/link.txt\n"
    b"rm /tmp/loop-a\nrm /tmp/loop-b\nrm /tmp/target.txt\n"
)
EXPECTED_LINKS_OUTPUT = (
    "usage: ln [-s] <target> <link>",
    "usage: readlink [-f] <path>",
    "readlink: /tmp/target.txt: not a symbolic link",
    "cat: cannot open /tmp/loop-a: Too many levels of symbolic links",
    "ln: /tmp/hard.txt: Operation not permitted",
    "cat: cannot open /tmp/link.txt: No such file or directory",
)
LINK_LINES = tuple(
    re.compile(line, re.MULTILINE)
    for line in (
        r"> cat /tmp/link\.txt\r?\nlinked text\r?$",
        r"> readlink /tmp/link\.txt\r?\ntarget\.txt\r?$",
        r"> readlink -f /tmp/link\.txt\r?\n/tmp/target\.txt\r?$",
        r"^lrwxrwxrwx +1 +0 +0 +10 1970-01-01 00:00 link\.txt"
        r" -> target\.txt\r?$",
        # rm took the link away and left its target.
        r"> rm /tmp/link\.txt\r?\n[^\n]*> cat /tmp/target\.txt\r?\n"
        r"linked text\r?$",
    )
)


def Check(result):
    for line in LINK_LINES:
        if line.search(result.output) is None:
            return f"the /tmp links part is missing {line.pattern!r}"
    return None


PART = Part(Lines(LINKS_INPUT), EXPECTED_LINKS_OUTPUT, Check)
