from smoke.cases.calc import Calc
from smoke.cases.exception import CpuException
from smoke.cases.interrupt import Interrupt
from smoke.cases.keyboard import Keyboard
from smoke.cases.memory import Memory
from smoke.cases.network import Network
from smoke.cases.normal_boot import NormalBoot
from smoke.cases.panic import Panic
from smoke.cases.power import Power
from smoke.cases.scheduler import Scheduler
from smoke.cases.shell import Shell
from smoke.cases.smp import Smp
from smoke.cases.storage import Storage
from smoke.cases.timer import Timer
from smoke.cases.user_preemption import UserPreemption

# Each --expect-<name> switch: its case and help text.
SWITCHES = {
    "panic": (Panic, "Expect the intentional panic test"),
    "exception": (CpuException, "Expect an intentional CPU exception"),
    "interrupt": (
        Interrupt,
        "Expect the architecture-specific hardware interrupt test",
    ),
    "memory": (Memory, "Expect the physical and virtual memory tests to pass"),
    "timer": (Timer, "Expect the architecture-specific periodic timer test"),
    "scheduler": (Scheduler, "Expect cooperative task switching to pass"),
    "smp": (Smp, "Expect tasks to run concurrently on several CPUs"),
    "network": (
        Network,
        "Expect the network device to resolve the gateway with ARP",
    ),
    "user-preemption": (
        UserPreemption,
        "Expect timer-driven user-task switching to pass",
    ),
    "input": (
        Keyboard,
        "Send serial input and expect the kernel to accept a line",
    ),
    "calc": (Calc, "Send a calculator command and expect its result"),
    "shell": (Shell, "Run shell commands and expect their output"),
}


def AddArguments(parser):
    for name, (_, help_text) in SWITCHES.items():
        parser.add_argument(
            f"--expect-{name}", action="store_true", help=help_text
        )
    parser.add_argument(
        "--expect-power",
        choices=("shutdown", "reboot"),
        help="Run the shell's shutdown or reboot and expect QEMU to exit or "
        "boot again",
    )
    parser.add_argument(
        "--expect-storage-boot",
        type=int,
        default=0,
        help="Expect a verified disk write that counts this many boots",
    )


def Select(parser, args):
    chosen = [
        case
        for name, (case, _) in SWITCHES.items()
        if getattr(args, "expect_" + name.replace("-", "_"))
    ]
    if args.expect_power:
        chosen.append(Power)
    if args.expect_storage_boot:
        chosen.append(Storage)
    if len(chosen) > 1:
        parser.error("select at most one expected result")
    return (chosen[0] if chosen else NormalBoot)(args)
