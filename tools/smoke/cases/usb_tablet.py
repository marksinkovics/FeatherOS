import re

from smoke.cases.usb_keys import FINAL_STEP, Button, MouseEvents, UsbKeys
from smoke.qemu import SendkeyNames

MULTILINE = re.MULTILINE


def Absolute(axis, value):
    return {"type": "abs", "data": {"axis": axis, "value": value}}


def Position(x, y):
    return MouseEvents([Absolute("x", x), Absolute("y", y)])


def Near(value):
    """The scaled value within one unit of the QEMU tablet's 0..32767."""
    return "|".join(
        str(number)
        for number in range(max(value - 1, 0), min(value + 1, 32767) + 1)
    )


def Axis(name, value):
    return rf"mouse: abs {name} (?:{Near(value)})\r?\n"


def Line(*parts):
    return re.compile(
        ("^" + "".join(parts) + r"mouse: syn\r?$").encode(), MULTILINE
    )


# Only the axes that changed are reported, so each corner after the first
# move changes one axis.
POSITIONS = (
    (Position(16383, 16383), Axis("x", 16383), Axis("y", 16383)),
    (Position(0, 0), Axis("x", 0), Axis("y", 0)),
    (Position(32767, 0), Axis("x", 32767)),
    (Position(32767, 32767), Axis("y", 32767)),
    (Position(0, 32767), Axis("x", 0)),
    (Position(0, 0), Axis("y", 0)),
)
EVENT_COUNT = 3 + 3 + 2 * 4 + 2 + 2 + 2


def TabletSteps():
    steps = [
        (
            SendkeyNames(f"mousetest {EVENT_COUNT}\n"),
            re.compile(rb"^mousetest: ready\r?$", MULTILINE),
        )
    ]
    for action, *axes in POSITIONS:
        steps.append((action, Line(*axes)))
    steps += [
        (
            MouseEvents([Button("wheel-up", True), Button("wheel-up", False)]),
            re.compile(rb"^mouse: rel wheel 1\r?\nmouse: syn\r?$", MULTILINE),
        ),
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


class UsbTablet(UsbKeys):
    """QEMU's usb-tablet reports absolute positions, which the kernel scales
    to 0..32767 and sends as EV_ABS records on /dev/mouse, with its buttons
    and wheel as for any mouse. The keyboard is only there to type
    mousetest."""

    mouse = "usb-tablet,bus=xhci.0,id=tablet1"
    mouse_name = "QEMU USB Tablet"
    mouse_driver = "hid"
    mouse_marker = b'"QEMU USB Tablet" hid'
    passed = (
        "the USB tablet's positions at the corners and the centre reached "
        "/dev/mouse as EV_ABS records within 0..32767, with its buttons and "
        "wheel"
    )

    def Steps(self):
        return [*TabletSteps(), FINAL_STEP]
