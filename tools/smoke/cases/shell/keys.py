from smoke.cases.shell.part import Part
from smoke.terminal import KEYS_READY

# keys reads raw input: every special key, Ctrl+C as a byte, then q to quit.
SHELL_KEYS_INPUT = (
    b"keys\n",
    b"\x1b[A\x1b[B\x1b[C\x1b[D\x1b[H\x1b[F\x1b[2~\x1b[3~\x1b[5~\x1b[6~"
    b"\x1b[Z\x1bOA\x1b[1~\t\x01\x03\x7f \rxq",
)
EXPECTED_KEYS = [
    KEYS_READY,
    "keys: up",
    "keys: down",
    "keys: right",
    "keys: left",
    "keys: home",
    "keys: end",
    "keys: insert",
    "keys: delete",
    "keys: page-up",
    "keys: page-down",
    "keys: shift-tab",
    "keys: up",
    "keys: home",
    "keys: tab",
    "keys: ctrl-a",
    "keys: ctrl-c",
    "keys: backspace",
    "keys: space",
    "keys: enter",
    "keys: x",
    "keys: done",
]


def Check(result):
    keys = [
        line.strip()
        for line in result.output.splitlines()
        if line.startswith("keys: ")
    ]
    if keys != EXPECTED_KEYS:
        return f"keys reported {keys}"
    return None


PART = Part(SHELL_KEYS_INPUT, (), Check)
