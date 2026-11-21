from smoke.boot import PANIC_MARKER
from smoke.cases.base import Case
from smoke.script import Lines

DUNGEON_TIMEOUT_SECONDS = 240
SELFTEST_LINE = "wickdeep: selftest ok"
# The same on both architectures: the rules use integers only.
DEMO_RESULTS = {
    1: "turns 2690 depth 8 level 7 score 1945 outcome victory "
    "hash 10821229194343717296",
    2: "turns 703 depth 3 level 2 score 223 outcome dead "
    "hash 18362298982438112458",
}
SAVE_PATH = "/tmp/w.sav"
SAVE_OK_LINE = "wickdeep: save ok depth 2 turn 500"
SAVE_REJECTED_LINE = "wickdeep: save rejected length"


def DemoLine(seed):
    return f"wickdeep: demo {DEMO_RESULTS[seed]}"


class Dungeon(Case):
    """Runs wickdeep's self-test and two demo runs (one wins, one dies), then
    saves a demo run at turn 500, checks the file and checks that it is
    refused with a reason once a byte is appended."""

    serial_input = True
    timeout = DUNGEON_TIMEOUT_SECONDS
    passed = (
        "wickdeep's self-test passed and its demo runs gave the expected "
        "turns, depth, score and state hashes, and its saved state was "
        "accepted, then refused after a byte was appended"
    )

    def Script(self, session):
        demos = "".join(
            f"wickdeep --demo --seed {seed}\n" for seed in DEMO_RESULTS
        )
        saves = (
            f"wickdeep --demo --seed 1 --save-at 500 {SAVE_PATH}\n"
            f"wickdeep --check-save {SAVE_PATH}\n"
            f"echo x >> {SAVE_PATH}\n"
            f"wickdeep --check-save {SAVE_PATH}\n"
        )
        return Lines(f"wickdeep --selftest\n{demos}{saves}".encode())

    def Markers(self):
        return (SAVE_REJECTED_LINE.encode(), PANIC_MARKER.encode())

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
        for expected in (SAVE_OK_LINE, SAVE_REJECTED_LINE):
            if expected not in lines:
                saves = [line for line in lines if "wickdeep: save" in line]
                return (
                    f"wickdeep --check-save gave no line {expected!r}: "
                    f"{saves}{result.limit_note}"
                )
        if PANIC_MARKER in result.output:
            return "the kernel panicked"
        return None
