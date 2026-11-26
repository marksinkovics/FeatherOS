import os
import re
import tempfile
import time

from smoke.qemu import (
    OpenQmpChannel,
    SendQmpCommand,
    SendQmpKeyEvent,
    SendQmpKeys,
)

WIDTH = 1024
HEIGHT = 768
DEFAULT_SCREEN = f"{WIDTH}x{HEIGHT}"
SCREEN_PATTERN = re.compile(r"(\d+)x(\d+)")
WHITE = 0xFFFFFF
BLACK = 0x000000
RETRY_SECONDS = 2.0
RETRY_PAUSE_SECONDS = 0.2


class StepFailed(Exception):
    pass


def ParseScreen(text):
    found = SCREEN_PATTERN.fullmatch(text)
    if found is None:
        raise ValueError(f"the screen size {text!r} is not WIDTHxHEIGHT")
    return int(found.group(1)), int(found.group(2))


def ReadPpm(path, size=(WIDTH, HEIGHT)):
    with open(path, "rb") as file:
        data = file.read()
    header = data.split(b"\n", 3)
    if header[0] != b"P6" or header[2] != b"255":
        raise RuntimeError("the screendump is not a binary PPM")
    width, height = (int(value) for value in header[1].split())
    if (width, height) != size:
        raise RuntimeError(f"the screendump is {width}x{height}")
    return header[3]


def PixelAt(pixels, x, y, width=WIDTH):
    offset = 3 * (y * width + x)
    return (
        (pixels[offset] << 16) | (pixels[offset + 1] << 8) | pixels[offset + 2]
    )


def Hex(color):
    return f"{color:06X}"


def Difference(pixels, expected, width=WIDTH):
    if pixels == expected:
        return None
    offset = next(
        index
        for index in range(len(pixels))
        if pixels[index] != expected[index]
    )
    x, y = divmod(offset // 3, width)[::-1]
    return (
        f"({x},{y}) is {Hex(PixelAt(pixels, x, y, width))}, expected "
        f"{Hex(PixelAt(expected, x, y, width))}"
    )


def Found(pixels, x0, y0, width, height, color):
    return any(
        PixelAt(pixels, x, y) == color
        for y in range(y0, y0 + height)
        for x in range(x0, x0 + width)
    )


def ExpectColor(pixels, what, point, wanted):
    found = PixelAt(pixels, *point)
    if found != wanted:
        return f"{what} at {point} is {Hex(found)}, expected {Hex(wanted)}"
    return None


class ScreenCase:
    """What a case needs to look at the screen and press keys through QMP:
    mix it in before Case, call InitScreen in the constructor, OpenScreen
    once the guest is up and CloseScreen from Stop. The screen is the case's
    --screen size, 1024x768 by default; a case that checks any other size
    sets any_screen."""

    any_screen = False

    def InitScreen(self):
        self.width, self.height = ParseScreen(self.args.screen)
        self.directory = tempfile.TemporaryDirectory(prefix="fq-")
        self.socket_path = os.path.join(self.directory.name, "qmp.sock")
        self.dump_path = os.path.join(self.directory.name, "screen.ppm")
        self.connection = None
        self.channel = None
        self.command_id = 2
        self.failure = None

    def QmpSocket(self):
        return self.socket_path

    def OpenScreen(self):
        self.connection, self.channel = OpenQmpChannel(self.socket_path)

    def CloseScreen(self):
        if self.channel is not None:
            self.channel.close()
        if self.connection is not None:
            self.connection.close()
        self.directory.cleanup()

    def ReadDump(self):
        return ReadPpm(self.dump_path, (self.width, self.height))

    def Dump(self, check, seconds=RETRY_SECONDS):
        deadline = time.monotonic() + seconds
        while True:
            self.command_id += 1
            SendQmpCommand(
                self.channel,
                self.command_id,
                "screendump",
                {"filename": self.dump_path},
            )
            failure = check(self.ReadDump())
            if failure is None or time.monotonic() >= deadline:
                return failure
            time.sleep(RETRY_PAUSE_SECONDS)

    def Probe(self, check):
        self.command_id += 1
        SendQmpCommand(
            self.channel,
            self.command_id,
            "screendump",
            {"filename": self.dump_path},
        )
        return check(self.ReadDump()) is None

    def Step(self, name, check, seconds=RETRY_SECONDS):
        failure = self.Dump(check, seconds)
        if failure is not None and self.failure is None:
            self.failure = f"{name}: {failure}"

    def Keys(self, keys):
        self.command_id = SendQmpKeys(self.channel, self.command_id + 1, keys)

    def Chord(self, *keys):
        for key, down in keys:
            self.command_id = SendQmpKeyEvent(
                self.channel, self.command_id, key, down
            )

    def Tap(self, *keys):
        self.Chord(*[(key, True) for key in keys])
        self.Chord(*[(key, False) for key in reversed(keys)])
