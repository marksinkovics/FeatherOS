from smoke.boot import PANIC_MARKER
from smoke.cases.base import Case
from smoke.script import Lines

DUNGEON_TIMEOUT_SECONDS = 240
SELFTEST_LINE = "wickdeep: selftest ok"
# The same on both architectures: the rules use integers only.
DEMO_RESULTS = {
    1: "turns 3684 depth 8 level 1 score 1200 outcome bottom "
    "hash 9680416461032995841",
    2: "turns 3146 depth 8 level 1 score 1200 outcome bottom "
    "hash 18330328682316093004",
}


def DemoLine(seed):
    return f"wickdeep: demo {DEMO_RESULTS[seed]}"


class Dungeon(Case):
    """Runs wickdeep's self-test and two demo runs and checks their exact
    output lines."""

    serial_input = True
    timeout = DUNGEON_TIMEOUT_SECONDS
    passed = (
        "wickdeep's self-test passed and its demo runs gave the expected "
        "turns, depth, score and state hashes"
    )

    def Script(self, session):
        demos = "".join(
            f"wickdeep --demo --seed {seed}\n" for seed in DEMO_RESULTS
        )
        return Lines(f"wickdeep --selftest\n{demos}".encode())

    def Markers(self):
        last = max(DEMO_RESULTS)
        return (DemoLine(last).encode(), PANIC_MARKER.encode())

    def Check(self, result):
        lines = result.output.replace("\r", "").split("\n")
        if SELFTEST_LINE not in lines:
            failed = [line for line in lines if "wickdeep: selftest" in line]
            return (
                f"wickdeep --selftest did not pass: {failed}"
                f"{result.limit_note}"
            )
        for seed in DEMO_RESULTS:
            if DemoLine(seed) not in lines:
                demos = [line for line in lines if "wickdeep: demo" in line]
                return (
                    f"wickdeep --demo --seed {seed} gave no line "
                    f"{DemoLine(seed)!r}: {demos}{result.limit_note}"
                )
        if PANIC_MARKER in result.output:
            return "the kernel panicked"
        return None
