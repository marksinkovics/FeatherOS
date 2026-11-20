import functools
import json
import os
import tempfile
import time

from smoke.boot import PANIC_MARKER
from smoke.cases.base import Case
from smoke.qemu import OpenQmpChannel, SendQmpCommand, SendQmpKeys
from smoke.script import Lines
from smoke.terminal import SHELL_PROMPT

GRAPHICS_TIMEOUT_SECONDS = 90
DEMO_SECONDS = 10
WIDTH = 1024
HEIGHT = 768
BAR_COLORS = (
    0xFF0000,
    0x00FF00,
    0x0000FF,
    0xFFFFFF,
    0xFFFF00,
    0x00FFFF,
    0xFF00FF,
    0x808080,
)
BAR_CENTERS = tuple((64 + 128 * index, 200) for index in range(8))
STATUS_POINT = (1011, 24)
PADDING_POINT = (4, 4)
# The corner rectangle, a pixel inside the gradient next to the patch, and the
# patch's corners.
SAMPLE_POINTS = BAR_CENTERS + (
    (0, 0),
    (9, 9),
    (10, 10),
    (999, 699),
    (1000, 700),
    (1023, 767),
    STATUS_POINT,
    PADDING_POINT,
)
WHITE = 0xFFFFFF
BLACK = 0x000000
RETRY_SECONDS = 2.0
RETRY_PAUSE_SECONDS = 0.2
PRESENTED = b"fbdemo: presented"
DONE = b"fbdemo: done"


def PatternAt(x, y):
    if -10 <= x < 10 and -10 <= y < 10:
        return 0x654321
    if x >= 1000 and y >= 700:
        return 0x123456
    if y < HEIGHT // 2:
        return BAR_COLORS[x // (WIDTH // 8)]
    return ((x // 4) << 16) | (((y - HEIGHT // 2) // 2) << 8)


def ReadPpm(path):
    with open(path, "rb") as file:
        data = file.read()
    header = data.split(b"\n", 3)
    if header[0] != b"P6" or header[2] != b"255":
        raise RuntimeError("the screendump is not a binary PPM")
    width, height = (int(value) for value in header[1].split())
    if (width, height) != (WIDTH, HEIGHT):
        raise RuntimeError(f"the screendump is {width}x{height}")
    return header[3]


def PixelAt(pixels, x, y):
    offset = 3 * (y * WIDTH + x)
    return (
        (pixels[offset] << 16) | (pixels[offset + 1] << 8) | pixels[offset + 2]
    )


def Hex(color):
    return f"{color:06X}"


def MatchingBars(pixels):
    return sum(
        PixelAt(pixels, x, y) == color
        for (x, y), color in zip(BAR_CENTERS, BAR_COLORS)
    )


@functools.cache
def PatternBytes():
    rows = []
    for y in range(HEIGHT):
        row = bytearray()
        for x in range(WIDTH):
            color = PatternAt(x, y)
            row += bytes((color >> 16, (color >> 8) & 0xFF, color & 0xFF))
        rows.append(bytes(row))
    return b"".join(rows)


def Difference(pixels, expected):
    if pixels == expected:
        return None
    offset = next(
        index
        for index in range(len(pixels))
        if pixels[index] != expected[index]
    )
    x, y = divmod(offset // 3, WIDTH)[::-1]
    return (
        f"({x},{y}) is {Hex(PixelAt(pixels, x, y))}, expected "
        f"{Hex(PixelAt(expected, x, y))}"
    )


# The whole screen, so anything the console drew over the pattern, such as the
# status bar's changing cells, shows.
def ExpectPattern(pixels):
    for x, y in SAMPLE_POINTS:
        found = PixelAt(pixels, x, y)
        if found != PatternAt(x, y):
            return (
                f"({x},{y}) is {Hex(found)}, expected {Hex(PatternAt(x, y))}"
            )
    return Difference(pixels, PatternBytes())


def ExpectBlank(pixels):
    return Difference(pixels, bytes(len(pixels)))


# A text console has text and the status bar, not the pattern. One bar center
# can be white because a glyph lands on it.
def ExpectText(pixels):
    found = PixelAt(pixels, *STATUS_POINT)
    if found != WHITE:
        return f"the status bar at (1011,24) is {Hex(found)}, expected FFFFFF"
    matching = MatchingBars(pixels)
    if matching > 1:
        return f"{matching} of 8 bar centers still show the pattern"
    return None


def ExpectTextAgain(pixels):
    found = PixelAt(pixels, *PADDING_POINT)
    if found != BLACK:
        return f"the padding at (4,4) is {Hex(found)}, expected 000000"
    return ExpectText(pixels)


class Graphics(Case):
    """Runs fbdemo on the first console and checks what QEMU's screendump
    shows while it owns the screen, on another console, after switching back,
    after drawing again and once it has released the screen."""

    serial_input = True
    timeout = GRAPHICS_TIMEOUT_SECONDS
    passed = (
        "fbdemo's pattern was drawn clipped without the status bar, the "
        "other console showed text, the blanked console stayed blank until "
        "it drew again, and releasing brought the text console back"
    )

    def __init__(self, args):
        super().__init__(args)
        self.directory = tempfile.TemporaryDirectory(prefix="fq-")
        self.socket_path = os.path.join(self.directory.name, "qmp.sock")
        self.dump_path = os.path.join(self.directory.name, "screen.ppm")
        self.connection = None
        self.channel = None
        self.command_id = 2
        self.state = 0
        self.failure = None
        self.finished = False

    def QmpSocket(self):
        return self.socket_path

    def Script(self, session):
        return Lines(f"fbdemo {DEMO_SECONDS}\n".encode())

    def Markers(self):
        return (PANIC_MARKER.encode(),)

    def Finished(self, clean_output):
        return self.finished

    def Dump(self, check):
        deadline = time.monotonic() + RETRY_SECONDS
        while True:
            self.command_id += 1
            SendQmpCommand(
                self.channel,
                self.command_id,
                "screendump",
                {"filename": self.dump_path},
            )
            failure = check(ReadPpm(self.dump_path))
            if failure is None or time.monotonic() >= deadline:
                return failure
            time.sleep(RETRY_PAUSE_SECONDS)

    def Step(self, name, check):
        failure = self.Dump(check)
        if failure is not None and self.failure is None:
            self.failure = f"{name}: {failure}"

    def Keys(self, keys):
        self.command_id = SendQmpKeys(self.channel, self.command_id + 1, keys)

    def OnOutput(self, session, clean_output):
        if self.finished:
            return
        try:
            if self.state == 0 and clean_output.count(PRESENTED) >= 1:
                self.connection, self.channel = OpenQmpChannel(
                    self.socket_path
                )
                # The status task has to tick before a wrongly drawn bar shows.
                time.sleep(1.5)
                self.Step("the pattern", ExpectPattern)
                self.Keys(("alt-f2",))
                self.Step("the second console", ExpectText)
                self.Keys(("alt-f1",))
                self.Step("back on the blanked console", ExpectBlank)
                session.process.stdin.write(b"r\n")
                session.process.stdin.flush()
                self.state = 1
            elif self.state == 1 and clean_output.count(PRESENTED) >= 2:
                self.Step("the second present", ExpectPattern)
                self.state = 2
            elif self.state == 2:
                done = clean_output.find(DONE)
                if done >= 0 and SHELL_PROMPT.search(clean_output, done):
                    self.Step("after release", ExpectTextAgain)
                    self.finished = True
        except (
            OSError,
            RuntimeError,
            ValueError,
            json.JSONDecodeError,
        ) as error:
            self.failure = f"screendump or key injection failed: {error}"
            self.finished = True

    def Stop(self):
        if self.channel is not None:
            self.channel.close()
        if self.connection is not None:
            self.connection.close()
        self.directory.cleanup()

    def Check(self, result):
        if self.failure is not None:
            return self.failure
        if "fbdemo: tty1 1024x768" not in result.output:
            return f"fbdemo did not start{result.limit_note}"
        if not self.finished:
            return f"the session ended early{result.limit_note}"
        if PANIC_MARKER in result.output:
            return "the kernel panicked"
        return None
