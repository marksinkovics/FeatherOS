import re

from smoke.cases.shell.part import Part

# Line editing at the shell prompt: Home and Right, Up to recall, Ctrl+U and
# Ctrl+W, Delete, and a lone Escape, which the editor ignores.
SHELL_EDIT_INPUT = (
    b"eco edited line\x1b[H\x1b[C\x1b[Ch\n",
    b"\x1b[A\n",
    b"echo wrong\x15echo right words\x17\x17after-kill\n",
    b"echo deleteXX\x1b[D\x1b[D\x1b[3~\x1b[3~ done\n",
    b"echo esc\x1b ok\n",
    b"history\n",
    b"clear\n",
)
CLEAR_SEQUENCE = b"\x1b[H\x1b[2J"
# Program output lines, which the edited input lines echoed before them do not
# match, and how often each must appear.
EXPECTED_SHELL_EDITS = {
    "edited line": 2,
    "after-kill": 1,
    "delete done": 1,
    "esc ok": 1,
}
EXPECTED_HISTORY_LINE = re.compile(
    r"^ +\d+  echo edited line\r?$", re.MULTILINE
)


def Check(result):
    edits = {
        text: len(re.findall(rf"^{text}\r?$", result.output, re.MULTILINE))
        for text in EXPECTED_SHELL_EDITS
    }
    if edits != EXPECTED_SHELL_EDITS or not EXPECTED_HISTORY_LINE.search(
        result.output
    ):
        return f"line editing printed {edits}"
    if CLEAR_SEQUENCE not in result.captured:
        return "clear did not clear the screen"
    return None


PART = Part(SHELL_EDIT_INPUT, (), Check)
