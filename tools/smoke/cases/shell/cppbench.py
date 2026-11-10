from smoke.cases.shell.part import Part
from smoke.script import Lines

PART = Part(
    Lines(b"cppbench -n 1\ncppbench -n 3 life json\ncppbench nothing\n"),
    (
        "life: check 691, 1 run in ",
        "sieve: check 78498, 1 run in ",
        "quicksort: check 2963021034, 1 run in ",
        "sort: check 2963021034, 1 run in ",
        "json: check 138999, 1 run in ",
        "life: check 691, 3 runs in ",
        "json: check 138999, 3 runs in ",
        "cppbench: unknown workload nothing; see cppbench -h",
    ),
)
