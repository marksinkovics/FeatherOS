import re

from smoke.cases.shell.part import Part
from smoke.script import Lines

WORKERS_INPUT = (
    b"primes 4 100000\nprimes 16 20000\n"
    b"threads 4 100000\nthreads 15 20000\n"
    b"fptest 8\n"
)
EXPECTED_WORKERS_OUTPUT = (
    "fptest: 8 of 8 workers kept their registers",
    "primes: 9592 primes below 100000 with 4 workers in ",
    # Sixteen workers keep sixteen pipes open at once.
    "primes: 2262 primes below 20000 with 16 workers in ",
    # Threads of one process find the same counts as worker processes.
    "threads: 9592 primes below 100000 with 4 threads in ",
    "threads: 2262 primes below 20000 with 15 threads in ",
)
THREADS_WORKER_OUTPUT = re.compile(
    r"^threads: thread \d+ found \d+ on CPU \d+(?:,\d+)*\r?$", re.MULTILINE
)
PRIMES_WORKER_OUTPUT = re.compile(
    r"^primes: worker \d+ found \d+ on CPU \d+(?:,\d+)*\r?$", re.MULTILINE
)


def Check(result):
    # primes 4 and primes 16 report one line per worker.
    if len(PRIMES_WORKER_OUTPUT.findall(result.output)) != 20:
        return "primes did not report every worker"
    if len(THREADS_WORKER_OUTPUT.findall(result.output)) != 19:
        return "threads did not report every thread"
    return None


PART = Part(Lines(WORKERS_INPUT), EXPECTED_WORKERS_OUTPUT, Check)
