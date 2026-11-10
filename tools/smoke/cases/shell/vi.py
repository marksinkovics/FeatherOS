import re

from smoke.cases.shell.part import Part
from smoke.script import TypeWhen

# vi creates a file, edits it with inserts, motions, a yank and put, undo, a
# search and r, and writes it; a second session refuses :q after a change and
# leaves with :q!. upper prints the file each time, in capitals that vi's own
# screen never shows.
SHELL_VI_INPUT = (
    b"vi /tmp/vi.txt\n",
    TypeWhen(
        '"/tmp/vi.txt" [New]',
        b"ihello world\x1bothird line\x1b0Osecond line\x1bgg$xuGyypddggJu"
        b"/third\nrT:wq\n",
    ),
    b"upper < /tmp/vi.txt\n",
    b"cd /tmp\n",
    b"vi vi.txt\n",
    TypeWhen('"vi.txt" 3L, 35B', b"x:q\n:q!\n"),
    b"upper < vi.txt\n",
    b"cd\n",
)
EXPECTED_VI_LINES = {"HELLO WORLD": 2, "SECOND LINE": 2, "THIRD LINE": 2}


def Check(result):
    vi_lines = {
        text: len(re.findall(rf"^{text}\r?$", result.output, re.MULTILINE))
        for text in EXPECTED_VI_LINES
    }
    if vi_lines != EXPECTED_VI_LINES:
        return f"vi wrote {vi_lines}"
    return None


PART = Part(
    SHELL_VI_INPUT, ("No write since last change (add ! to override)",), Check
)
