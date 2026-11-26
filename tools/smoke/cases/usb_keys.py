import json
import os
import re
import tempfile

from smoke.boot import DHCP_LEASED_MESSAGE, PANIC_MARKER
from smoke.cases.base import INPUT_TIMEOUT_SECONDS
from smoke.cases.normal_boot import NormalBoot
from smoke.qemu import (
    OpenQmpChannel,
    SendkeyNames,
    SendQmpCommand,
    SendQmpKeys,
)
from smoke.terminal import SHELL_PROMPT

MULTILINE = re.MULTILINE
# Each step types keys and waits for the line they should produce. The input
# goes to QEMU's first keyboard, which is the USB one because the machine has
# no other.
KEY_STEPS = (
    (
        SendkeyNames("echo usb works | upper\n"),
        re.compile(rb"^USB WORKS\r?$", MULTILINE),
    ),
    # Caps Lock is a global state toggled once per press: a and b in capitals,
    # then c and d in lower case.
    (
        (
            *SendkeyNames("echo "),
            "caps_lock",
            "a",
            "b",
            "caps_lock",
            "c",
            "d",
            "ret",
        ),
        re.compile(rb"^ABcd\r?$", MULTILINE),
    ),
    # Ctrl+C at the prompt discards the line typed so far.
    (
        ("x", "y", "z", "ctrl-c", *SendkeyNames("echo ctrl c | upper\n")),
        re.compile(rb"^CTRL C\r?$", MULTILINE),
    ),
    # Left then z gives "echo azb"; Up recalls it.
    (
        (*SendkeyNames("echo ab"), "left", "z", "ret"),
        re.compile(rb"^azb\r?$", MULTILINE),
    ),
    (("up", "ret"), re.compile(rb"^azb\r?$", MULTILINE)),
)
FINAL_STEP = (
    SendkeyNames("echo usb keys done | upper\n"),
    re.compile(rb"^USB KEYS DONE\r?$", MULTILINE),
)


class MouseEvents(list):
    """QMP input-send-event events for QEMU's active mouse, which is the USB
    one because the machine has no other pointing device."""


def Relative(axis, value):
    return {"type": "rel", "data": {"axis": axis, "value": value}}


def Button(button, down):
    return {"type": "btn", "data": {"button": button, "down": down}}


def MouseSteps():
    count = 9
    steps = [
        (
            SendkeyNames(f"mousetest {count}\n"),
            re.compile(rb"^mousetest: ready\r?$", MULTILINE),
        ),
        (
            MouseEvents([Relative("x", 10), Relative("y", -5)]),
            re.compile(
                rb"^mouse: rel x 10\r?\nmouse: rel y -5\r?\nmouse: syn\r?$",
                MULTILINE,
            ),
        ),
    ]
    steps.append(
        (
            MouseEvents([Button("wheel-up", True), Button("wheel-up", False)]),
            re.compile(rb"^mouse: rel wheel 1\r?\nmouse: syn\r?$", MULTILINE),
        )
    )
    steps += [
        (
            MouseEvents([Button("left", True)]),
            re.compile(rb"^mouse: key 272 1\r?\nmouse: syn\r?$", MULTILINE),
        ),
        (
            MouseEvents([Button("left", False)]),
            re.compile(rb"^mouse: key 272 0\r?\nmouse: syn\r?$", MULTILINE),
        ),
    ]
    return steps


DONE = "USB KEYS DONE"
CONTROLLER_LINE = re.compile(
    r"KERN/I: USB: xHCI 1b36:000d at [0-9a-f]{2}:[0-9a-f]{2}\.[0-7], "
    r"\d+ ports, 64 slots, (?:32|64)-byte contexts, (MSI-X|MSI|INTx|polling)"
)
FAILURES = (
    "did not reset",
    "could not be addressed",
    "enumeration failed",
    "could not configure",
    "could not select",
    "driver could not attach",
    "too many transfer errors",
    "host controller error",
)


class UsbKeys(NormalBoot):
    """Keys typed on QEMU's usb-kbd reach the shell: the x86-64 machine has
    no i8042 and the AArch64 one no virtio keyboard, so the USB keyboard is
    the only one. Typed text, Caps Lock, Ctrl+C and the cursor keys with
    history all work."""

    timeout = INPUT_TIMEOUT_SECONDS
    serial_input = False
    virtio_keyboard = False
    controller = "qemu-xhci,id=xhci"
    keyboard = "usb-kbd,bus=xhci.0,id=kbd1"
    mouse = "usb-mouse,bus=xhci.0,id=mouse1"
    mouse_name = "QEMU USB Mouse"
    mouse_driver = "hid-mouse"
    mouse_marker = b"hid-mouse"
    interrupts = "MSI-X"
    speed = "high"
    passed = (
        "the USB keyboard typed text, Caps Lock, Ctrl+C and the cursor keys "
        "into the shell, and the USB mouse's moves and buttons reached "
        "/dev/mouse, wheel included"
    )

    def __init__(self, args):
        super().__init__(args)
        self.directory = tempfile.TemporaryDirectory(prefix="fq-")
        self.socket_path = os.path.join(self.directory.name, "qmp.sock")
        self.connection = None
        self.channel = None
        self.command_id = 2
        self.steps = self.Steps()
        self.step = 0
        self.position = None
        self.redrawn = False
        self.error = None

    def Steps(self):
        return [*KEY_STEPS, *MouseSteps(), FINAL_STEP]

    def QmpSocket(self):
        return self.socket_path

    def QemuArguments(self):
        arguments = [
            "-device",
            self.controller,
            "-device",
            self.keyboard,
            "-device",
            self.mouse,
        ]
        if self.args.arch == "x86_64":
            arguments = ["-machine", "pc,i8042=off", *arguments]
        return arguments

    def Markers(self):
        return (DONE.encode(), PANIC_MARKER.encode())

    def SendAction(self, action):
        if isinstance(action, MouseEvents):
            SendQmpCommand(
                self.channel,
                self.command_id,
                "input-send-event",
                {"events": list(action)},
            )
            self.command_id += 1
        else:
            self.command_id = SendQmpKeys(
                self.channel, self.command_id, action
            )

    def OnOutput(self, session, clean_output):
        try:
            self.Advance(clean_output)
        except (OSError, RuntimeError, json.JSONDecodeError) as error:
            self.error = str(error)
            self.step = len(self.steps)

    def Advance(self, clean_output):
        if self.step >= len(self.steps):
            return
        if self.position is None:
            lease = clean_output.find(DHCP_LEASED_MESSAGE)
            # Enumeration can finish after the shell is up, and keys or mouse
            # events sent before their device is bound go nowhere.
            if (
                lease < 0
                or b"hid-keyboard" not in clean_output
                or self.mouse_marker not in clean_output
            ):
                return
            if self.channel is None:
                self.connection, self.channel = OpenQmpChannel(
                    self.socket_path
                )
            if SHELL_PROMPT.search(clean_output, lease):
                self.position = len(clean_output)
                self.SendAction(self.steps[0][0])
            elif not self.redrawn and SHELL_PROMPT.search(clean_output):
                self.SendAction(("ret",))
                self.redrawn = True
            return
        if (
            self.steps[self.step][1].search(clean_output, self.position)
            is None
        ):
            return
        self.step += 1
        self.position = len(clean_output)
        if self.step < len(self.steps):
            self.SendAction(self.steps[self.step][0])

    def Stop(self):
        if self.channel is not None:
            self.channel.close()
        if self.connection is not None:
            self.connection.close()
        self.directory.cleanup()

    def Check(self, result):
        failure = super().Check(result)
        if failure is not None:
            return failure
        output = result.output
        if self.error is not None:
            return f"could not type through QMP: {self.error}"
        controller = CONTROLLER_LINE.search(output)
        if controller is None:
            return "the boot log has no xHCI line"
        if controller.group(1) != self.Interrupts():
            return (
                f"the controller used {controller.group(1)}, not "
                f"{self.Interrupts()}"
            )
        device = re.search(
            rf"KERN/I: USB 1-\d+(?:\.\d+)*: {self.speed}-speed 0627:0001 "
            r"\"QEMU USB Keyboard\" hid-keyboard",
            output,
        )
        if device is None:
            return f"no {self.speed}-speed USB keyboard bound to hid-keyboard"
        if not re.search(
            rf"KERN/I: USB 1-\d+(?:\.\d+)*: {self.speed}-speed "
            rf"[0-9a-f]{{4}}:[0-9a-f]{{4}} \"{self.mouse_name}\" "
            rf"{self.mouse_driver}(?![-\w])",
            output,
        ):
            return (
                f"no {self.speed}-speed {self.mouse_name} bound to "
                f"{self.mouse_driver}"
            )
        for warning in FAILURES:
            if warning in output:
                return f"the log reports a USB failure: {warning}"
        if self.step < len(self.steps):
            return (
                f"step {self.step + 1} of {len(self.steps)} never produced "
                f"its line{result.limit_note}"
            )
        return None

    def Interrupts(self):
        return self.interrupts


class UsbFallback(UsbKeys):
    """The same typing and mouse with everything the defaults avoid: a
    controller without MSI or MSI-X (INTx on AArch64, polling on x86-64),
    full-speed devices, and the kernel told to use the report descriptor
    instead of the boot layout."""

    controller = "qemu-xhci,id=xhci,msi=off,msix=off"
    keyboard = "usb-kbd,bus=xhci.0,id=kbd1,usb_version=1"
    mouse = "usb-mouse,bus=xhci.0,id=mouse1,usb_version=1"
    speed = "full"
    passed = (
        "typing and the mouse, wheel included, worked on full-speed devices "
        "through the report parser, with the controller's interrupts as the "
        "target allows without MSI"
    )

    def Interrupts(self):
        return "INTx" if self.args.arch == "aarch64" else "polling"
