from smoke.cases.shell.part import Part
from smoke.script import (
    CONTROL_Z,
    INTERRUPT_RUNNING,
    SUSPEND_RUNNING,
    TypeWhen,
)

# Ctrl+Z stops a sleep, which bg continues for the time it had left and fg
# waits for, then stops a pipeline, which fg continues and Ctrl+C ends. A
# background upper stops with SIGTTIN when it reads, and reads once fg gives it
# the console. vi, top and a shell started by the shell stop themselves on
# Ctrl+Z and come back as they were with fg.
SHELL_STOP_INPUT = (
    b"sleep 4\n",
    SUSPEND_RUNNING,
    b"jobs\n",
    b"bg\n",
    b"jobs\n",
    b"fg\n",
    b"sleep 100 | sleep 100\n",
    SUSPEND_RUNNING,
    b"jobs\n",
    b"fg 1\n",
    INTERRUPT_RUNNING,
    b"upper &\n",
    b"sleep 0.5\n",
    b"jobs\n",
    b"fg\n",
    b"read after fg\n",
    b"vi /tmp/stop.txt\n",
    TypeWhen('"/tmp/stop.txt" [New]', b"istopped vi\x1b\x1a"),
    b"jobs\n",
    b"fg\n",
    TypeWhen('"/tmp/stop.txt" [New]', b":wq\n"),
    b"upper < /tmp/stop.txt\n",
    b"top\n",
    SUSPEND_RUNNING,
    b"fg\n",
    TypeWhen("; q quits", b"q"),
    # The shell redraws its prompt from the row's start, which does not count
    # as a prompt here, so exit waits in the input for it.
    b"shell\n",
    CONTROL_Z,
    b"fg\nexit\n",
)
EXPECTED_STOP_OUTPUT = (
    "[1] Stopped  sleep 4",
    "[1] sleep 4 &",
    "[1] Running  sleep 4",
    "sleep: slept 4 s",
    "[1] Stopped  sleep 100 | sleep 100",
    "[1] Stopped  upper &",
    "\nREAD AFTER FG",
    "[1] Stopped  vi /tmp/stop.txt",
    "\nSTOPPED VI",
    "[1] Stopped  top",
    "[1] Stopped  shell",
)

# The stopped pipeline's Ctrl+C; ^Z for the sleep, the pipeline and the
# shell's line editor, while vi and top echo none.
PART = Part(SHELL_STOP_INPUT, EXPECTED_STOP_OUTPUT, interrupts=1, suspends=3)
