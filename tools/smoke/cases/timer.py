from smoke.cases.base import BootToShell

EXPECTED_TIMER = "KERN/TIMER: periodic ticks passed"


class Timer(BootToShell):
    passed = "periodic timer test reported success over serial"

    def Check(self, result):
        if EXPECTED_TIMER not in result.output:
            return "periodic timer test did not report success"
        return None
