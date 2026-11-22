import re

from smoke.cases.usb_keys import FINAL_STEP, KEY_STEPS, MouseSteps, UsbKeys
from smoke.qemu import SendkeyNames, SendQmpCommand

MULTILINE = re.MULTILINE
HOTPLUG_ROUNDS = 3
HOTPLUG_PORT = "3"
LISTED = "LSUSB LISTED"
HUB_LINE = re.compile(
    r"KERN/I: USB 1-(\d+): (low|full|high|super)-speed ([0-9a-f]{4}):"
    r"([0-9a-f]{4}) \"([^\"]*)\" hub"
)
LISTING_LINE = re.compile(
    r"^(1-\d+(?:\.\d+)* dev (\d+) (?:low|full|high|super) "
    r"[0-9a-f]{4}:[0-9a-f]{4} class [0-9a-f]{2}/[0-9a-f]{2}/[0-9a-f]{2} "
    r"\S+)\r?$",
    MULTILINE,
)
FAILURES = (
    "did not reset",
    "did not enable",
    "did not settle",
    "too many hub tiers",
    "over-current",
    "hub has more ports",
)


class QmpCommand:
    """A QMP command a step sends instead of keys."""

    def __init__(self, execute, arguments):
        self.execute = execute
        self.arguments = arguments


def ListingStep():
    return (
        (
            *SendkeyNames("lsusb\n"),
            *SendkeyNames("echo lsusb listed | upper\n"),
        ),
        re.compile(rf"^{LISTED}\r?$".encode(), MULTILINE),
    )


def HotplugSteps(rounds):
    steps = []
    for number in range(1, rounds + 1):
        steps += [
            (
                QmpCommand(
                    "device_add",
                    {
                        "driver": "usb-kbd",
                        "bus": "xhci.0",
                        "port": f"1.{HOTPLUG_PORT}",
                        "id": "k2",
                    },
                ),
                re.compile(
                    rb"KERN/I: USB 1-\d+\."
                    + HOTPLUG_PORT.encode()
                    + rb": (?:low|full|high)-speed 0627:0001 "
                    rb"\"QEMU USB Keyboard\" hid-keyboard"
                ),
            ),
            (
                SendkeyNames(f"echo k2 {number} | upper\n"),
                re.compile(rf"^K2 {number}\r?$".encode(), MULTILINE),
            ),
            (
                QmpCommand("device_del", {"id": "k2"}),
                re.compile(
                    rb"KERN/I: USB 1-\d+\."
                    + HOTPLUG_PORT.encode()
                    + rb": disconnected"
                ),
            ),
        ]
    return steps


class UsbHub(UsbKeys):
    """The keyboard and the mouse sit behind QEMU's usb-hub (QEMU ports 1.1
    and 1.2): the kernel enumerates the hub, then its two children by route,
    typing and the mouse work as without a hub, and the log names the devices
    1-N.1 and 1-N.2. A third keyboard is then plugged into hub port 3, typed
    on and pulled out three times through QMP, and lsusb shows the starting
    list again."""

    hub = "usb-hub,bus=xhci.0,port=1,id=hub1"
    keyboard = "usb-kbd,bus=xhci.0,port=1.1,id=kbd1"
    mouse = "usb-mouse,bus=xhci.0,port=1.2,id=mouse1"
    # QEMU's hub is a full-speed device, and so are the ports behind it.
    speed = "full"
    passed = (
        "the keyboard and mouse behind a hub were enumerated by route, typed "
        "and moved the mouse, and a keyboard hot-plugged into a hub port "
        "typed three times and left lsusb as it was"
    )

    def __init__(self, args):
        super().__init__(args)
        self.steps = [
            *KEY_STEPS,
            *MouseSteps(),
            ListingStep(),
            *HotplugSteps(HOTPLUG_ROUNDS),
            ListingStep(),
            FINAL_STEP,
        ]

    def QemuArguments(self):
        arguments = super().QemuArguments()
        # The hub goes in front of the devices that name its ports.
        index = arguments.index(self.keyboard) - 1
        return [*arguments[:index], "-device", self.hub, *arguments[index:]]

    def SendAction(self, action):
        if isinstance(action, QmpCommand):
            SendQmpCommand(
                self.channel, self.command_id, action.execute, action.arguments
            )
            self.command_id += 1
        else:
            super().SendAction(action)

    def Check(self, result):
        failure = super().Check(result)
        if failure is not None:
            return failure
        output = result.output
        for warning in FAILURES:
            if warning in output:
                return f"the log reports a hub failure: {warning}"
        hubs = HUB_LINE.findall(output)
        if len(hubs) != 1:
            return f"expected one hub line, found {hubs}"
        root = hubs[0][0]
        for child, product, driver in (
            ("1", "QEMU USB Keyboard", "hid-keyboard"),
            ("2", "QEMU USB Mouse", "hid-mouse"),
        ):
            line = (
                f"USB 1-{root}.{child}: {self.speed}-speed 0627:0001 "
                f'"{product}" {driver}'
            )
            if line not in output:
                return f"no USB 1-{root}.{child} line for {product}"
        added = output.count(f"USB 1-{root}.{HOTPLUG_PORT}: ")
        gone = output.count(f"USB 1-{root}.{HOTPLUG_PORT}: disconnected")
        if gone != HOTPLUG_ROUNDS or added != 2 * HOTPLUG_ROUNDS:
            return (
                f"hub port {HOTPLUG_PORT} logged {added} lines and "
                f"{gone} disconnections for {HOTPLUG_ROUNDS} rounds"
            )
        listing = [match[0] for match in LISTING_LINE.findall(output)]
        if len(listing) != 6:
            return f"lsusb printed {len(listing)} lines, not 6: {listing}"
        if listing[:3] != listing[3:]:
            return f"lsusb after the hot-plug differs: {listing}"
        names = [line.split(" ")[0] for line in listing[:3]]
        if names != [f"1-{root}", f"1-{root}.1", f"1-{root}.2"]:
            return f"lsusb lists {names}"
        return None
