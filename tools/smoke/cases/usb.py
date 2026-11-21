import re

from smoke.boot import NETWORK_DHCP_MESSAGE, PANIC_MARKER
from smoke.cases.normal_boot import NormalBoot
from smoke.script import WaitFor

CONTROLLER_LINE = re.compile(
    r"KERN/I: USB: xHCI 1b36:000d at [0-9a-f]{2}:[0-9a-f]{2}\.[0-7], "
    r"(\d+) ports, 64 slots, (32|64)-byte contexts, MSI-X"
)
DEVICE_LINE = re.compile(
    r"KERN/I: USB: port (\d+) (low|full|high|super)-speed device "
    r"addressed as slot (\d+)"
)
LAST_DEVICE = "device addressed as slot 2"
DEVICES = 2
FAILURES = (
    "did not reset",
    "could not be addressed",
    "no device slots left",
    "got no device slot",
    "could not set up its device slot",
    "host controller error",
    "refused:",
)


class Usb(NormalBoot):
    """QEMU's xHCI controller with a keyboard and a mouse on it: the controller
    comes up with MSI-X on both targets and the kernel reaches Address Device
    for both devices on their root ports."""

    passed = (
        "xHCI came up with MSI-X and both devices were addressed on their "
        "root ports"
    )

    def QemuArguments(self):
        return [
            "-device",
            "qemu-xhci,id=xhci",
            "-device",
            "usb-kbd,bus=xhci.0",
            "-device",
            "usb-mouse,bus=xhci.0",
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
        if len(devices) != DEVICES:
            return f"expected {DEVICES} addressed devices, found {devices}"
        ports = {port for port, _, _ in devices}
        slots = {slot for _, _, slot in devices}
        if len(ports) != DEVICES or slots != {"1", "2"}:
            return f"the devices share a port or slot: {devices}"
        if int(max(ports, key=int)) > int(controller.group(1)):
            return "a device sits on a port the controller does not have"
        for warning in FAILURES:
            if warning in output:
                return f"the log reports a USB failure: {warning}"
        return None

    def PassMessage(self, result):
        devices = DEVICE_LINE.findall(result.output)
        speeds = ", ".join(
            f"port {port} {speed}-speed" for port, speed, _ in devices
        )
        return f"{self.passed} ({speeds})"
