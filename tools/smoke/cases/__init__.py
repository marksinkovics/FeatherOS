from smoke.cases.calc import Calc
from smoke.cases.dungeon import Dungeon
from smoke.cases.exception import CpuException
from smoke.cases.fonts import Fonts
from smoke.cases.graphics import Graphics
from smoke.cases.interrupt import Interrupt
from smoke.cases.keyboard import Keyboard
from smoke.cases.memory import Memory
from smoke.cases.network import Network
from smoke.cases.no_disk import NoDisk
from smoke.cases.normal_boot import NormalBoot
from smoke.cases.panic import Panic
from smoke.cases.pci import Pci
from smoke.cases.power import Power
from smoke.cases.root import RootOption
from smoke.cases.scheduler import Scheduler
from smoke.cases.shell import Shell
from smoke.cases.smp import Smp
from smoke.cases.storage import Storage
from smoke.cases.timer import Timer
from smoke.cases.usb import Usb
from smoke.cases.usb_hub import UsbHub
from smoke.cases.usb_keys import UsbFallback, UsbKeys
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
    "graphics": (
        Graphics,
        "Run fbdemo and check the screen with QEMU's screendump",
    ),
    "dungeon": (Dungeon, "Run wickdeep's self-test and demo runs"),
    "fonts": (
        Fonts,
        "Print UTF-8 text and compare the screen with the font's glyphs",
    ),
    "pci": (Pci, "Expect edu to deliver an MSI and a DMA round trip"),
    "usb": (
        Usb,
        "Expect the xHCI controller to address a keyboard and a mouse",
    ),
    "usb-keys": (
        UsbKeys,
        "Type on a USB keyboard and expect the shell to receive it",
    ),
    "usb-fallback": (
        UsbFallback,
        "Type on a full-speed USB keyboard through the report parser, "
        "without MSI",
    ),
    "usb-hub": (
        UsbHub,
        "Type and move the mouse behind a USB hub, and hot-plug a keyboard "
        "into one of its ports",
    ),
    "nodisk": (
        NoDisk,
        "Boot with no disk and expect a RAM root, a shell and a shutdown",
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
        "--expect-root",
        metavar="vdaN",
        help="The kernel was given root=vdaN: expect that ext2 or ext3 "
        "partition mounted and written, or a warning and the default root "
        "for any other name",
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
    if args.expect_root:
        chosen.append(RootOption)
    if args.expect_storage_boot:
        chosen.append(Storage)
    if len(chosen) > 1:
        parser.error("select at most one expected result")
    return (chosen[0] if chosen else NormalBoot)(args)
