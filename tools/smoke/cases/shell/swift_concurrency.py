import re

from smoke.cases.shell.part import Part
from smoke.script import Lines

# Swift concurrency, built where deps/ holds the runtime's sources.
SWIFT_CONCURRENCY_INPUT = b"tasks\ntaskprimes 16 100000\nexecutors\n"
EXPECTED_SWIFT_CONCURRENCY_OUTPUT = (
    "tasks: top-level code on the main thread true, assumeIsolated true",
    "tasks: a task returned 42",
    "tasks: turns 3 pings and 3 pongs, each in order",
    "tasks: 100 increments counted 100",
    "tasks: squares of 1 to 10 sum to 385",
    "tasks: async let gave 25",
    "tasks: a continuation resumed with 7",
    "tasks: a stream of 1 to 5 sums to 15",
    "tasks: a buffered stream of 1 to 100 sums to 5050",
    "tasks: cancelled after ",
    "tasks: the error came through true",
    "tasks: task-local outer, task outer, detached none",
    "tasks: slept 20, 30 and 50 ms for at least 100 ms",
    "tasks: a sleep of 60 s ended early with CancellationError",
    "tasks: checkCancellation threw CancellationError",
    "tasks: main actor from a worker: worker true, @MainActor function true, "
    "MainActor.run true, @MainActor task true, detached task false",
    "tasks: still on the main thread true",
    "taskprimes: 9592 primes below 100000 with 16 tasks in ",
    "executors: an actor ran 100 calls on its own thread true, "
    "resumed there after sleeping true, "
    "top-level code on the main thread true",
    "executors: assumeIsolated outside a job: "
    "isIsolatingCurrentContext true, checkIsolated true",
    "executors: 16 tasks preferring a pool ran there true",
    "executors: preferring the pool: a function true, async let true, "
    "a default actor true, the pinned actor on its thread true, "
    "Task {} false, globalConcurrentExecutor false",
    "executors: Task(executorPreference:) on the pool true",
)
# The global executor runs jobs on every CPU; QEMU has 4, so the tasks spread.
TASK_PRIMES_SPREAD_OUTPUT = re.compile(
    r"^taskprimes: the tasks ran on (\d+) threads "
    r"and CPU (\d+(?:,\d+)*)\r?$",
    re.MULTILINE,
)


def Check(result):
    spread = TASK_PRIMES_SPREAD_OUTPUT.search(result.output)
    if (
        not spread
        or int(spread.group(1)) < 2
        or len(spread.group(2).split(",")) < 2
    ):
        return (
            "taskprimes did not run on several threads and "
            f"CPUs: {spread.group(0).strip() if spread else 'none'}"
        )
    return None


PART = Part(
    Lines(SWIFT_CONCURRENCY_INPUT),
    EXPECTED_SWIFT_CONCURRENCY_OUTPUT,
    Check,
    requires="tasks",
    missing_note="Swift concurrency was not built; skipping its checks",
)
