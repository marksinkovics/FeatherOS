import functools
import json
import os
import re
import select
import tempfile
import time

from smoke.boot import PANIC_MARKER
from smoke.cases.base import Case
from smoke.qemu import (
    OpenQmpChannel,
    SendQmpCommand,
    SendQmpKeyEvent,
    SendQmpKeys,
)
from smoke.script import Lines
from smoke.terminal import (
    ANSI_ESCAPE_BYTES,
    CONSOLE_REPORT,
    LEAVE_ALTERNATE_SCREEN,
    SHELL_PROMPT,
)

GRAPHICS_TIMEOUT_SECONDS = 150
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
KEYS_COMMAND = "fbdemo --keys 120\n"
SELFTEST_COMMAND = "featherfall --selftest\n"
SELFTEST_LINE = "featherfall: selftest ok"
# The same on both architectures: the game's rules use integers only.
DEMO_RESULTS = {
    1: "score 82553 lines 104 level 11 hash 6636180809909333056",
    2: "score 84561 lines 106 level 11 hash 385706885443829982",
}
EVENT_WAIT_SECONDS = 3.0
QUIET_SECONDS = 0.5
# Key events a console queues before dropping: the queue's 64 less the two
# slots kept for Shown and Hidden.
KEY_EVENT_LIMIT = 62
OVERFLOW_PAIRS = 100
TOKEN = re.compile(
    r"^fbdemo: (?:event (\w+) (\d+) (\d+)|(presented|paused|resumed|"
    r"interrupted|time went backwards|dropped \d+))\r?\n",
    re.MULTILINE,
)
SHELL_PROMPT_TEXT = re.compile(SHELL_PROMPT.pattern.decode(), re.MULTILINE)


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


class StepFailed(Exception):
    pass


class EventStream:
    """The lines fbdemo --keys prints, read one at a time from the session
    while the harness drives the keyboard."""

    def __init__(self, session):
        self.session = session
        self.origin = 0
        self.taken = 0

    def Text(self):
        captured = bytes(self.session.captured)
        clean = ANSI_ESCAPE_BYTES.sub(
            b"",
            CONSOLE_REPORT.sub(b"", captured).replace(
                LEAVE_ALTERNATE_SCREEN, b"\n"
            ),
        )
        return clean.decode("utf-8", errors="replace")

    def Start(self, marker):
        text = self.Text()
        first = text.find(marker)
        self.origin = text.find(marker, first + 1)
        if self.origin < 0:
            raise StepFailed(f"{marker!r} did not appear twice")
        self.taken = 0

    def Tokens(self):
        tokens = []
        for match in TOKEN.finditer(self.Text(), self.origin):
            if match.group(1) is not None:
                tokens.append(" ".join(match.group(1, 2, 3)))
            else:
                tokens.append(match.group(4))
        return tokens

    def Pump(self, seconds):
        stdout = self.session.process.stdout
        readable, _, _ = select.select([stdout], [], [], seconds)
        if not readable:
            return
        chunk = os.read(stdout.fileno(), 4096)
        if not chunk:
            raise StepFailed("the guest's output ended")
        self.session.captured.extend(chunk)

    def Next(self, seconds=EVENT_WAIT_SECONDS):
        deadline = time.monotonic() + seconds
        while True:
            if PANIC_MARKER in self.Text():
                raise StepFailed("the kernel panicked")
            tokens = self.Tokens()
            if len(tokens) > self.taken:
                self.taken += 1
                return tokens[self.taken - 1]
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            self.Pump(remaining)

    def Expect(self, step, *wanted, seconds=EVENT_WAIT_SECONDS):
        for pattern in wanted:
            found = self.Next(seconds)
            if found is None or re.fullmatch(pattern, found) is None:
                raise StepFailed(
                    f"{step}: expected {pattern!r}, got {found!r}"
                )

    def Collect(self, quiet=QUIET_SECONDS, limit=10.0):
        deadline = time.monotonic() + limit
        found = []
        while time.monotonic() < deadline:
            token = self.Next(quiet)
            if token is None:
                break
            found.append(token)
        return found

    def Quiet(self, step):
        found = self.Collect(QUIET_SECONDS, QUIET_SECONDS)
        if found:
            raise StepFailed(f"{step}: unexpected {found!r}")

    def WaitText(self, pattern, seconds=EVENT_WAIT_SECONDS):
        deadline = time.monotonic() + seconds
        while True:
            found = pattern.search(self.Text(), self.origin)
            if found is not None:
                return found
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            self.Pump(remaining)


class Graphics(Case):
    """Runs fbdemo on the first console and checks what QEMU's screendump
    shows while it owns the screen, on another console, after switching back,
    after drawing again and once it has released the screen; then runs
    fbdemo --keys and checks the key events QMP key presses produce."""

    serial_input = True
    timeout = GRAPHICS_TIMEOUT_SECONDS
    passed = (
        "fbdemo's pattern was drawn clipped without the status bar, the "
        "other console showed text, the blanked console stayed blank until "
        "it drew again, releasing brought the text console back, key "
        "events, switching, overflow and Ctrl+C behaved as specified, and "
        "featherfall's self-test and demo runs gave the expected results"
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
        demos = "".join(
            f"featherfall --demo --seed {seed}\n" for seed in DEMO_RESULTS
        )
        return Lines(
            f"{SELFTEST_COMMAND}{demos}fbdemo {DEMO_SECONDS}\n"
            f"{KEYS_COMMAND}".encode()
        )

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

    def Chord(self, *keys):
        for key, down in keys:
            self.command_id = SendQmpKeyEvent(
                self.channel, self.command_id, key, down
            )

    def Tap(self, *keys):
        self.Chord(*[(key, True) for key in keys])
        self.Chord(*[(key, False) for key in reversed(keys)])

    def RunKeys(self, session):
        events = EventStream(session)
        events.Start("fbdemo: tty1")
        events.Expect("startup", "presented", "shown 0 0", "presented")

        self.Chord(("a", True), ("a", False))
        events.Expect("a", "press 30 0", "release 30 0")

        self.Chord(("shift", True), ("a", True), ("a", False))
        self.Chord(("shift", False))
        events.Expect(
            "shift", "press 42 1", "press 30 1", "release 30 1", "release 42 0"
        )

        self.Chord(("left", True), ("left", True), ("left", True))
        self.Chord(("left", False))
        events.Expect(
            "repeat",
            "press 105 0",
            "repeat 105 0",
            "repeat 105 0",
            "release 105 0",
        )

        self.Chord(("ctrl", True), ("z", True), ("z", False))
        self.Chord(("ctrl", False))
        events.Expect(
            "ctrl+z",
            "press 29 2",
            "press 44 2",
            "release 44 2",
            "release 29 0",
        )

        self.Chord(("x", True), ("alt", True))
        events.Expect("held key", "press 45 0", "press 56 4")
        self.Chord(("f2", True))
        events.Expect(
            "switching away",
            r"release 45 \d+",
            r"release 56 \d+",
            "hidden 0 0",
        )
        self.Step("fbdemo's console hidden", ExpectText)
        self.Chord(("f2", False), ("alt", False), ("x", False))
        events.Quiet("keys on the other console")

        self.Chord(("alt", True), ("f1", True))
        events.Expect("switching back", "shown 0 0", "presented")
        self.Chord(("f1", False), ("alt", False))
        events.Quiet("after switching back")
        self.Step("switched back", ExpectPattern)

        session.process.stdin.write(b"p\n")
        session.process.stdin.flush()
        events.Expect("pause", "paused")
        for _ in range(OVERFLOW_PAIRS):
            self.Chord(("a", True), ("a", False))
        events.Expect("pause", "resumed", seconds=15)
        queued = events.Collect()
        wanted = ["press 30 0", "release 30 0"] * (KEY_EVENT_LIMIT // 2)
        if queued != wanted:
            raise StepFailed(
                f"overflow: {len(queued)} events, expected "
                f"{KEY_EVENT_LIMIT} alternating press and release"
            )

        self.Chord(("ctrl", True), ("c", True), ("c", False))
        self.Chord(("ctrl", False))
        events.Expect("ctrl+c", "press 29 2", "press 46 2")
        while True:
            token = events.Next()
            if token == "interrupted":
                break
            if token not in ("release 46 2", "release 29 0"):
                raise StepFailed(
                    f"ctrl+c: expected interrupted, got {token!r}"
                )
        events.Expect("ctrl+c", r"dropped \d+")
        if events.WaitText(SHELL_PROMPT_TEXT) is None:
            raise StepFailed("the prompt did not return after Ctrl+C")
        self.Step("after Ctrl+C", ExpectTextAgain)
        dropped = int(re.findall(r"fbdemo: dropped (\d+)", events.Text())[-1])
        if dropped < 1:
            raise StepFailed("overflow: no event was counted as dropped")

        session.process.stdin.write(b"echo ok\n")
        session.process.stdin.flush()
        if events.WaitText(re.compile(r"> echo ok\r?\nok\r?\n")) is None:
            raise StepFailed("the shell line after Ctrl+C was not 'echo ok'")
        if "time went backwards" in events.Text():
            raise StepFailed("event times went backwards")

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
                    self.state = 3
            elif self.state == 3 and clean_output.count(b"fbdemo: tty1") >= 2:
                self.state = 4
                try:
                    self.RunKeys(session)
                except StepFailed as failure:
                    if self.failure is None:
                        self.failure = str(failure)
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
        output = result.output.replace("\r", "")
        if SELFTEST_LINE not in output.split("\n"):
            return f"featherfall --selftest did not pass{result.limit_note}"
        for seed, results in DEMO_RESULTS.items():
            line = f"featherfall: demo seed {seed} {results}"
            if line not in output.split("\n"):
                return (
                    f"featherfall --demo --seed {seed} gave no line {line!r}"
                )
        if "fbdemo: tty1 1024x768" not in result.output:
            return f"fbdemo did not start{result.limit_note}"
        if not self.finished:
            return f"the session ended early{result.limit_note}"
        if PANIC_MARKER in result.output:
            return "the kernel panicked"
        return None
