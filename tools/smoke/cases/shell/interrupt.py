from smoke.cases.shell.part import Part
from smoke.script import CONTROL_C, INTERRUPT_RUNNING

# Interrupts a CPU-bound job, a sleep, programs blocked reading input, and the
# shell's own prompt (which only discards the line).
SHELL_INTERRUPT_INPUT = (
    b"primes 1 1000000000\n",
    INTERRUPT_RUNNING,
    # The reader of an interrupted writer sees its input end and exits too.
    b"primes 1 1000000000 | upper\n",
    INTERRUPT_RUNNING,
    # Ctrl+C reaches every command in the foreground group.
    b"sleep 100 | sleep 100\n",
    INTERRUPT_RUNNING,
    b"sleep 100\n",
    INTERRUPT_RUNNING,
    b"upper\n",
    CONTROL_C,
    b"calc\n",
    CONTROL_C,
    CONTROL_C,
    b"calc 1 +\n",
)
# The output of the last line, which ends the session.
END_MARKER = "exit status 1"

PART = Part(
    SHELL_INTERRUPT_INPUT,
    ("calc: unexpected end of expression", END_MARKER),
    interrupts=7,
)
