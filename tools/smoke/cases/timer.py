from smoke.cases.base import BootToShell

EXPECTED_TIMER = "KERN/TIMER: periodic ticks passed"
EXPECTED_FINE_CLOCK = "KERN/TIMER: fine clock passed"


class Timer(BootToShell):
    passed = "periodic timer test reported success over serial"

    def Check(self, result):
        if EXPECTED_TIMER not in result.output:
            return "periodic timer test did not report success"
        if EXPECTED_FINE_CLOCK not in result.output:
            return "fine clock test did not report success"
        return None
