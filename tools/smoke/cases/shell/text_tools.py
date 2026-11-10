import re

from smoke.cases.shell.part import Part
from smoke.script import Lines

# The text tools, on files and through pipes.
TEXT_TOOLS_INPUT = (
    b"write /tmp/w.txt zeta\nwrite -a /tmp/w.txt alpha\n"
    b"write -a /tmp/w.txt mu\nwrite -a /tmp/w.txt alpha\n"
    b"sort -u /tmp/w.txt\ncat /tmp/w.txt | sort -r\n"
    b"grep -n alpha /tmp/w.txt\ncat /tmp/w.txt | grep -c -i ALPHA\n"
    b"grep -v alpha /tmp/w.txt\nwc /tmp/w.txt\nhead -n 2 /tmp/w.txt\n"
    b"cat /tmp/w.txt | tail -n 2\ncp /tmp/w.txt /tmp/copy.txt\n"
    b"wc -l /tmp/copy.txt\ntouch /tmp/new.txt\nwc -c /tmp/new.txt\n"
    b"write /tmp/n.txt 10\nwrite -a /tmp/n.txt 9\n"
    b"write -a /tmp/n.txt 100\nsort -n /tmp/n.txt\n"
    b"sort -rn /tmp/n.txt\ngrep nothing /tmp/n.txt\n"
    # grep takes basic regular expressions, extended ones with -E and plain
    # text with -F.
    b"write /tmp/g.txt key=42 id=7\nwrite -a /tmp/g.txt plain line\n"
    b"write -a /tmp/g.txt a.b\nwrite -a /tmp/g.txt axb\n"
    b"grep -n a.b /tmp/g.txt\ngrep -F -v -n a.b /tmp/g.txt\n"
    b'grep "^p.*e$" /tmp/g.txt\ngrep -E "^[a-z]+=[0-9]{2} " /tmp/g.txt\n'
    b'grep -E -o "[a-z]+=[0-9]+" /tmp/g.txt\n'
    b'grep -o -i -E "(X|Q)B$" /tmp/g.txt\ngrep -E "(" /tmp/g.txt\n'
    # mv renames within a filesystem, and copies between /tmp and the disk.
    b"mkdir /tmp/box\nmv /tmp/copy.txt /tmp/box\nmv /tmp/box /tmp/crate\n"
    b"wc -l /tmp/crate/copy.txt\nmv /tmp/crate /tmp/crate/inner\n"
    b"mv /tmp/crate/copy.txt work\nwc -l /tmp/crate/copy.txt\n"
    b'mv work/copy.txt "work/Moved Copy.txt"\nwc -w "work/moved copy.txt"\n'
)
# Each pattern spans the lines one text-tool command printed, in order.
EXPECTED_TEXT_TOOLS = (
    r"^alpha\r?\nmu\r?\nzeta\r?$",
    r"^zeta\r?\nmu\r?\nalpha\r?\nalpha\r?$",
    r"^2:alpha\r?\n4:alpha\r?$",
    r"^2\r?$",
    r"^zeta\r?\nmu\r?$",
    r"^4 4 20 /tmp/w\.txt\r?$",
    r"^zeta\r?\nalpha\r?$",
    r"^mu\r?\nalpha\r?$",
    r"^4 /tmp/copy\.txt\r?$",
    r"^0 /tmp/new\.txt\r?$",
    r"^9\r?\n10\r?\n100\r?$",
    r"^100\r?\n10\r?\n9\r?$",
    r"^4 /tmp/crate/copy\.txt\r?$",
    r"^mv: /tmp/crate: Invalid argument\r?$",
    r"^wc: /tmp/crate/copy\.txt: No such file or directory\r?$",
    r"^4 work/moved copy\.txt\r?$",
    r"^3:a\.b\r?\n4:axb\r?$",
    r"^1:key=42 id=7\r?\n2:plain line\r?\n4:axb\r?$",
    r"^plain line\r?$",
    r"^key=42 id=7\r?$",
    r"^key=42\r?\nid=7\r?$",
    r"^xb\r?$",
    r"^grep: Unmatched \( or \\\(\r?$",
)


def Check(result):
    missing = [
        pattern
        for pattern in EXPECTED_TEXT_TOOLS
        if not re.search(pattern, result.output, re.MULTILINE)
    ]
    if missing:
        return f"text tools did not print {missing}"
    return None


PART = Part(Lines(TEXT_TOOLS_INPUT), (), Check)
