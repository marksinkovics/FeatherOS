import re

from smoke.boot import NETWORK_DHCP_MESSAGE, PANIC_MARKER
from smoke.cases.normal_boot import NormalBoot
from smoke.script import WaitFor

CONTROLLER_LINE = re.compile(
    r"KERN/I: USB: xHCI 1b36:000d at [0-9a-f]{2}:[0-9a-f]{2}\.[0-7], "
    r"(\d+) ports, 64 slots, (32|64)-byte contexts, MSI-X"
)
DEVICE_LINE = re.compile(
    r"KERN/I: USB 1-(\d+): (low|full|high|super)-speed "
    r"([0-9a-f]{4}):([0-9a-f]{4}) \"([^\"]*)\""
)
LAST_DEVICE = 'full-speed 0627:0001 "QEMU USB Keyboard"'
DEVICES = [
    ("high", "QEMU USB Keyboard"),
    ("high", "QEMU USB Mouse"),
    ("full", "QEMU USB Keyboard"),
]
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
)


class Usb(NormalBoot):
    """QEMU's xHCI controller with a keyboard and a mouse at high speed and a
    keyboard at full speed: the controller comes up with MSI-X on both targets
    and the kernel enumerates each device on its root port down to the
    descriptors, logging its speed, IDs and product name."""

    passed = (
        "xHCI came up with MSI-X and all three devices were enumerated on "
        "their root ports"
    )

    def QemuArguments(self):
        return [
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
        return [WaitFor(NETWORK_DHCP_MESSAGE), WaitFor(LAST_DEVICE)]

    def Check(self, result):
        failure = super().Check(result)
        if failure is not None:
            return failure
        output = result.output
        if PANIC_MARKER in output:
            return "the kernel panicked"
        controller = CONTROLLER_LINE.search(output)
        if controller is None:
            return "the boot log has no xHCI line with MSI-X"
        if "USB: interrupts by polling" in output:
            return "the controller fell back to polling"
        devices = DEVICE_LINE.findall(output)
        found = sorted((speed, product) for _, speed, _, _, product in devices)
        if found != sorted(DEVICES):
            return f"expected devices {sorted(DEVICES)}, found {found}"
        ports = {port for port, _, _, _, _ in devices}
        if len(ports) != len(DEVICES):
            return f"the devices share a port: {devices}"
        if int(max(ports, key=int)) > int(controller.group(1)):
            return "a device sits on a port the controller does not have"
        for _, _, vendor, product, _ in devices:
            if (vendor, product) != ("0627", "0001"):
                return f"unexpected device ID {vendor}:{product}"
        for warning in FAILURES:
            if warning in output:
                return f"the log reports a USB failure: {warning}"
        return None

    def PassMessage(self, result):
        devices = DEVICE_LINE.findall(result.output)
        speeds = ", ".join(
            f"port {port} {speed}-speed {product}"
            for port, speed, _, _, product in devices
        )
        return f"{self.passed} ({speeds})"
