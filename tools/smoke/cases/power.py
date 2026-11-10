from smoke.boot import PANIC_MARKER, SHELL_BANNER
from smoke.cases.base import SHELL_TIMEOUT_SECONDS, Case

POWER_MESSAGES = {
    "shutdown": (
        "KERN/I: Shutdown: stopping processes",
        "KERN/I: Shutdown: disk flushed",
        "KERN/I: Powering off",
    ),
    "reboot": (
        "KERN/I: Shutdown: stopping processes",
        "KERN/I: Shutdown: disk flushed",
        "KERN/I: Restarting",
    ),
}


class Power(Case):
    timeout = SHELL_TIMEOUT_SECONDS
    serial_input = True

    def __init__(self, args):
        super().__init__(args)
        self.kind = args.expect_power
        self.reboots = self.kind == "reboot"
        self.shuts_down = self.kind == "shutdown"

    def Script(self, session):
        return [f"{self.kind}\n".encode()]

    # A shutdown ends when QEMU exits; a reboot when the shell is back.
    def Markers(self):
        return (PANIC_MARKER.encode(),)

    def Finished(self, clean_output):
        return self.reboots and clean_output.count(SHELL_BANNER.encode()) >= 2

    def Check(self, result):
        output = result.output
        missing = [
            text for text in POWER_MESSAGES[self.kind] if text not in output
        ]
        boots = output.count(
            "KERN/I: Limine transferred control to the kernel"
        )
        if missing or PANIC_MARKER in output:
            return f"power output is missing {missing}{result.limit_note}"
        if self.shuts_down and not result.exited_by_itself:
            return "QEMU kept running after shutdown"
        if self.reboots and boots < 2:
            return "the system did not boot again"
        return None

    def PassMessage(self, result):
        outcome = "powered off" if self.shuts_down else "booted again"
        return (
            f"{self.kind} stopped processes, flushed the disk, and {outcome}"
        )
