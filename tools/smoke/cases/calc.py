from smoke.boot import PANIC_MARKER
from smoke.cases.base import Case
from smoke.script import Lines

EXPECTED_CALC = "69.25"
EXPECTED_UNKNOWN_COMMAND = "Unknown command: nope"
CALC_INPUT = b"nope\n\ncalc 4 + 66 - (3 / 4)\n"


class Calc(Case):
    serial_input = True
    passed = "unknown command rejected; calculator returned 69.25"

    def Script(self, session):
        return Lines(CALC_INPUT)

    def Markers(self):
        return (EXPECTED_CALC.encode(), PANIC_MARKER.encode())

    def Check(self, result):
        if EXPECTED_CALC not in result.output:
            return "calculator command result was not observed"
        if result.output.count(EXPECTED_UNKNOWN_COMMAND) != 1:
            return "expected exactly one unknown-command message"
        return None
