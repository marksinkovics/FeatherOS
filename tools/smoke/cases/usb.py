import json
import os
import re
import tempfile

from smoke.boot import NETWORK_DHCP_MESSAGE, PANIC_MARKER
from smoke.cases.base import INPUT_TIMEOUT_SECONDS
from smoke.cases.normal_boot import NormalBoot
from smoke.qemu import (
    OpenQmpChannel,
    SendkeyNames,
    SendQmpCommand,
    SendQmpKeys,
)
from smoke.script import WaitFor, WaitUntil

CONTROLLER_LINE = re.compile(
    r"KERN/I: USB: xHCI 1b36:000d at [0-9a-f]{2}:[0-9a-f]{2}\.[0-7], "
    r"(\d+) ports, 64 slots, (32|64)-byte contexts, MSI-X"
)
DEVICE_LINE = re.compile(
    r"KERN/I: USB 1-(\d+): (low|full|high|super)-speed "
    r"([0-9a-f]{4}):([0-9a-f]{4}) \"([^\"]*)\" (hid-keyboard|hid-mouse)"
)
ADDED_LINE = re.compile(
    rb"KERN/I: USB 1-(\d+): high-speed 0627:0001 \"QEMU USB Keyboard\" "
    rb"hid-keyboard"
)
LISTING_LINE = re.compile(
    r"^(1-\d+ dev (\d+) (?:low|full|high|super) ([0-9a-f]{4}):([0-9a-f]{4}) "
    r"class ([0-9a-f]{2}/[0-9a-f]{2}/[0-9a-f]{2}) (\S+))\r?$",
    re.MULTILINE,
)
LAST_DEVICE = 'full-speed 0627:0001 "QEMU USB Keyboard" hid-keyboard'
LISTED = "LSUSB LISTED"
DONE = "USB DONE"
DEVICES = [
    ("high", "QEMU USB Keyboard"),
    ("high", "QEMU USB Mouse"),
    ("full", "QEMU USB Keyboard"),
]
# QEMU's USB 2 port 4 is the controller's root port 8; the boot devices sit
# on QEMU's ports 1 to 3.
HOTPLUG_PORT = "4"
HOTPLUG_ROUNDS = 3
FAILURES = (
    "did not reset",
    "could not be addressed",
    "no device slots left",
    "got no device slot",
    "could not set up its device slot",
    "enumeration failed",
    "went away during enumeration",
    "invalid control endpoint packet size",
    "refused",
    "host controller error",
    "could not configure",
    "could not select",
    "driver could not attach",
)


class Usb(NormalBoot):
    """QEMU's xHCI controller with a keyboard and a mouse at high speed and a
    keyboard at full speed: the controller comes up with MSI-X on both targets
    and the kernel enumerates each device on its root port down to the
    descriptors, logging its speed, IDs, product name and the HID driver that
    bound. lsusb lists them with their drivers; then a keyboard is plugged
    into a free root port, typed on (QEMU sends keys to the keyboard added
    last) and pulled out three times through QMP, each time logging its line
    and its disconnection, and lsusb shows the starting list again."""

    timeout = INPUT_TIMEOUT_SECONDS
    serial_input = True
    virtio_keyboard = False
    passed = (
        "xHCI came up with MSI-X, all three devices were enumerated on "
        "their root ports, bound to the HID drivers and listed, and a "
        "keyboard was hot-plugged and typed on three times"
    )

    def __init__(self, args):
        super().__init__(args)
        self.directory = tempfile.TemporaryDirectory(prefix="fq-")
        self.socket_path = os.path.join(self.directory.name, "qmp.sock")
        self.connection = None
        self.channel = None
        self.command_id = 2
        self.phase = "waiting"
        self.round = 0
        self.added_before = 0
        self.removed_before = 0
        self.port = None
        self.ports = []
        self.error = None

    def QmpSocket(self):
        return self.socket_path

    def QemuArguments(self):
        machine = (
            ["-machine", "pc,i8042=off"] if self.args.arch == "x86_64" else []
        )
        return [
            *machine,
            "-device",
            "qemu-xhci,id=xhci",
            "-device",
            "usb-kbd,bus=xhci.0",
            "-device",
            "usb-mouse,bus=xhci.0",
            "-device",
            "usb-kbd,bus=xhci.0,usb_version=1",
        ]

    def Script(self, session):
        return [
            WaitFor(NETWORK_DHCP_MESSAGE),
            WaitFor(LAST_DEVICE),
            b"lsusb\n",
            b"echo lsusb listed | upper\n",
            WaitUntil(lambda: self.phase == "done"),
            b"lsusb\n",
            b"echo usb done | upper\n",
        ]

    def Markers(self):
        return (DONE.encode(), PANIC_MARKER.encode())

    def Command(self, execute, arguments=None):
        SendQmpCommand(self.channel, self.command_id, execute, arguments)
        self.command_id += 1

    def Type(self, text):
        self.command_id = SendQmpKeys(
            self.channel, self.command_id, SendkeyNames(text)
        )

    def OnOutput(self, session, clean_output):
        try:
            self.Advance(clean_output)
        except (OSError, RuntimeError, json.JSONDecodeError) as error:
            self.error = str(error)
            self.phase = "done"

    def Advance(self, clean_output):
        if self.phase == "waiting" and LISTED.encode() in clean_output:
            self.connection, self.channel = OpenQmpChannel(self.socket_path)
            self.phase = "plug"
        if self.phase == "plug":
            self.added_before = len(ADDED_LINE.findall(clean_output))
            self.Command(
                "device_add",
                {
                    "driver": "usb-kbd",
                    "bus": "xhci.0",
                    "port": HOTPLUG_PORT,
                    "id": "k2",
                },
            )
            self.phase = "plugged"
            return
        if self.phase == "plugged":
            added = ADDED_LINE.findall(clean_output)
            if len(added) <= self.added_before:
                return
            self.port = added[-1].decode()
            self.ports.append(self.port)
            self.removed_before = clean_output.count(
                f"USB 1-{self.port}: disconnected".encode()
            )
            self.Type(f"echo k2 {self.round + 1} | upper\n")
            self.phase = "typed"
            return
        if self.phase == "typed":
            if (
                re.search(
                    rf"^K2 {self.round + 1}\r?$".encode(),
                    clean_output,
                    re.MULTILINE,
                )
                is None
            ):
                return
            self.Command("device_del", {"id": "k2"})
            self.phase = "unplugged"
            return
        if self.phase == "unplugged":
            removed = clean_output.count(
                f"USB 1-{self.port}: disconnected".encode()
            )
            if removed <= self.removed_before:
                return
            self.round += 1
            self.phase = "plug" if self.round < HOTPLUG_ROUNDS else "done"
            # The next step waits for output, and none may come before it.
            self.Advance(clean_output)

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
            return f"could not hot-plug through QMP: {self.error}"
        if PANIC_MARKER in output:
            return "the kernel panicked"
        controller = CONTROLLER_LINE.search(output)
        if controller is None:
            return "the boot log has no xHCI line with MSI-X"
        if "USB: interrupts by polling" in output:
            return "the controller fell back to polling"
        devices = DEVICE_LINE.findall(output)[: len(DEVICES)]
        found = sorted(
            (speed, product) for _, speed, _, _, product, _ in devices
        )
        if found != sorted(DEVICES):
            return f"expected devices {sorted(DEVICES)}, found {found}"
        ports = {device[0] for device in devices}
        if len(ports) != len(DEVICES):
            return f"the devices share a port: {devices}"
        if int(max(ports, key=int)) > int(controller.group(1)):
            return "a device sits on a port the controller does not have"
        for _, _, vendor, product, _, _ in devices:
            if (vendor, product) != ("0627", "0001"):
                return f"unexpected device ID {vendor}:{product}"
        for warning in FAILURES:
            if warning in output:
                return f"the log reports a USB failure: {warning}"
        return self.CheckHotplug(output)

    def CheckHotplug(self, output):
        if self.round != HOTPLUG_ROUNDS:
            return (
                f"hot-plug stopped after {self.round} of {HOTPLUG_ROUNDS} "
                f"rounds in phase {self.phase}"
            )
        if len(set(self.ports)) != 1:
            return f"the hot-plugged keyboard moved between ports {self.ports}"
        port = self.ports[0]
        if output.count(f"USB 1-{port}: disconnected") != HOTPLUG_ROUNDS:
            return f"USB 1-{port} was not disconnected {HOTPLUG_ROUNDS} times"
        listing = [match[0] for match in LISTING_LINE.findall(output)]
        if len(listing) != 2 * len(DEVICES):
            return f"lsusb printed {len(listing)} lines: {listing}"
        before, after = listing[: len(DEVICES)], listing[len(DEVICES) :]
        if before != after:
            return f"lsusb after the hot-plug differs: {before} then {after}"
        entries = LISTING_LINE.findall(output)[: len(DEVICES)]
        numbers = sorted(int(entry[1]) for entry in entries)
        if numbers != [2, 3, 4]:
            return f"device numbers are {numbers}, not 2, 3 and 4"
        bound = sorted((entry[4], entry[5]) for entry in entries)
        expected = [
            ("03/01/01", "hid-keyboard"),
            ("03/01/01", "hid-keyboard"),
            ("03/01/02", "hid-mouse"),
        ]
        if bound != expected:
            return f"unexpected classes and drivers {bound}"
        return None

    def PassMessage(self, result):
        devices = DEVICE_LINE.findall(result.output)[: len(DEVICES)]
        speeds = ", ".join(
            f"port {port} {speed}-speed {product}"
            for port, speed, _, _, product, _ in devices
        )
        return f"{self.passed} ({speeds})"
