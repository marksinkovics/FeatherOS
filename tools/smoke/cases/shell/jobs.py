import re

from smoke.cases.shell.part import Part
from smoke.script import Lines

JOBS_INPUT = (
    b"upper &\nsleep 1 &\njobs\nwait\nkill %1\nwait\nsleep 0.8 &\nfg\n"
    # A background job that starts a program of its own must not take the
    # console from the shell.
    b"primes 1 1000000 > /dev/null &\njobs\n"
    b"echo the shell kept the console\nwait\n"
    b"kill -l\nkill 99999\nkill -TERM 1\n"
    # top must show a busy background worker using most of a CPU, which
    # kill %1 then stops.
    b"ps\nprimes 1 1000000000 > /dev/null &\ntop -n 1 -d 500\nkill %1\nwait\n"
    b"kill %7\n"
)
EXPECTED_JOBS_OUTPUT = (
    # Background jobs: upper stops when it reads the console, and kill %1 ends
    # it although it is stopped.
    "Running  sleep 1 &",
    "Done  sleep 1 &",
    "Stopped  upper &",
    "Terminated  upper &",
    "\nsleep 0.8 &",
    "\nthe shell kept the console",
    "Done  primes 1 1000000 > /dev/null &",
    " 9 KILL",
    "kill: 99999: no such process",
    "kill: 1: Operation not permitted",
    "  PID  PPID  PGID S CPU     TIME NAME",
    "  PID S  %CPU CPU     TIME NAME",
    "top: up ",
    "Terminated  primes 1 1000000000 > /dev/null &",
    "kill: %7: no such job",
)


def Check(result):
    busy = [
        int(percent)
        for percent in re.findall(
            r"^\s*\d+ R\s+(\d+)\.\d\s+\d+\s+\S+ primes\r?$",
            result.output,
            re.MULTILINE,
        )
    ]
    if not busy or max(busy) < 50:
        return f"top did not show a busy primes worker: {busy}"
    return None


PART = Part(Lines(JOBS_INPUT), EXPECTED_JOBS_OUTPUT, Check)
