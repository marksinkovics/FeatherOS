import re

from smoke.cases.shell.part import Part
from smoke.script import INTERRUPT_RUNNING, SUSPEND_RUNNING, TypeWhen

# script runs a shell on a pty, where line editing, jobs, Ctrl+C, Ctrl+Z, vi,
# top and size changes work as on the console, while the console shell keeps
# its own job.
# A nested script starts at the outer pty's size and follows its changes,
# which stty -F makes from inside; the outer pty is the only other pair, 0.
SHELL_SCRIPT_INPUT = (
    b"sleep 61 &\n",
    b"script /tmp/session.txt\n",
    b"eco edited in script | upper\x1b[H\x1b[C\x1b[Ch\n",
    b"sleep 2 &\n",
    b"jobs\n",
    b"fg\n",
    b"sleep 100\n",
    INTERRUPT_RUNNING,
    b"sleep 99\n",
    SUSPEND_RUNNING,
    b"jobs\n",
    b"fg\n",
    INTERRUPT_RUNNING,
    b"vi /tmp/sv.txt\n",
    TypeWhen('"/tmp/sv.txt" [New]', b"ivi in script\x1b\x1a"),
    b"fg\n",
    TypeWhen('"/tmp/sv.txt" [New]', b":wq\n"),
    b"top\n",
    SUSPEND_RUNNING,
    b"fg\n",
    TypeWhen("; q quits", b"q"),
    b"upper < /tmp/sv.txt\n",
    b"stty rows 20 cols 50\n",
    b"stty size\n",
    b'script -q -c "stty size" /tmp/inner.txt\n',
    b"script -q /tmp/inner.txt\n",
    b"stty -F /dev/pts/0 rows 22 cols 60\n",
    b"stty size\n",
    b"exit\n",
    b"exit\n",
    b"jobs\n",
    b"kill %1\n",
    b"wait\n",
    b"grep -n EDITED /tmp/session.txt\n",
    b"grep Script /tmp/session.txt | upper\n",
)
EXPECTED_SCRIPT_OUTPUT = (
    "Script started, output file is /tmp/session.txt",
    "[1] Running  sleep 2 &",
    "[1] Stopped  sleep 99",
    "Stopped  vi /tmp/sv.txt",
    "Stopped  top",
    "\nVI IN SCRIPT",
    "Script done, output file is /tmp/session.txt",
    "[1] Running  sleep 61 &",
    "[1] Terminated  sleep 61 &",
    "\nSCRIPT STARTED ON 20",
    "\nSCRIPT DONE ON 20",
)
# Each pattern and how often it must match: stty size inside script and in the
# nested script, then after the outer pty's size changed; the line edited
# inside script, on screen and in its file.
EXPECTED_SCRIPT_LINES = {
    r"^20 50\r?$": 2,
    r"^22 60\r?$": 1,
    r"^EDITED IN SCRIPT\r?$": 1,
    r"^\d+:EDITED IN SCRIPT\r*$": 1,
}


def Check(result):
    script_lines = {
        pattern: len(re.findall(pattern, result.output, re.MULTILINE))
        for pattern in EXPECTED_SCRIPT_LINES
    }
    if script_lines != EXPECTED_SCRIPT_LINES:
        return f"script printed {script_lines}"
    return None


# The two sleeps interrupted inside script; vi and top echo no ^Z.
PART = Part(
    SHELL_SCRIPT_INPUT, EXPECTED_SCRIPT_OUTPUT, Check, interrupts=2, suspends=1
)
