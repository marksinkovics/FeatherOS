import functools
import json
import os
import re
import select
import time

from smoke.boot import PANIC_MARKER
from smoke.cases.base import Case
from smoke.cases.fonts import ExpectRenderedText, FontFile, LoadFfnt
from smoke.cases.screen import (
    BLACK,
    HEIGHT,
    WHITE,
    WIDTH,
    Difference,
    ExpectColor,
    Found,
    Hex,
    PixelAt,
    ScreenCase,
    StepFailed,
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
# featherfall's layout (src/user/featherfall/render.swift) on the 1024x768
# screen: the well is 10x20 cells of 32 pixels, centered, between two panels.
GAME_SEED = 7
GAME_START_SECONDS = 8.0
CELL = 32
WELL_X = (WIDTH - 10 * CELL) // 2
WELL_Y = (HEIGHT - 20 * CELL) // 2
LEFT_PANEL_X = WELL_X - 24 - 216
BACKGROUND = 0x12141F
PANEL_FILL = 0x1B1F2E
PANEL_BORDER = 0x3A4160
WELL_FILL = 0x0A0C14
GRID = 0x131724
WELL_BORDER = 0x6C7596
TEXT = 0xE8EAF2
ACCENT = 0xFFC857
OVERLAY_FILL = 0x1E2338
PIECE_COLORS = (
    0x35C6D4,
    0xF2C94C,
    0xA66BDB,
    0x6FCF73,
    0xEB5757,
    0x4F7BE8,
    0xF2994A,
)
# Text boxes in the well, from the fonts' advances: sans-48 "PAUSED" is 163
# pixels wide and 48 tall, centered in the 320-pixel well; sans-32 "ABC" is 54
# wide. An initials slot is 64x80, three of them 16 apart, centered.
PAUSED_BOX = (WELL_X + (10 * CELL - 163) // 2, WELL_Y + 270, 163, 48)
FIRST_INITIALS_BOX = (WELL_X + 68, WELL_Y + 160, 54, 32)
SLOT_X = (10 * CELL - (3 * 64 + 2 * 16)) // 2
SLOT_STEP = 64 + 16
SLOT_SIZE = (64, 80)
# The three numbers of the left panel are mono-32 text of nine cells of 16x32,
# padded with spaces on the right, in a box filled with the panel's color.
NUMBER_X = LEFT_PANEL_X + 16
NUMBER_ROWS = tuple(WELL_Y + 202 + 90 * index for index in range(3))
NUMBER_CELLS = 9
NUMBER_CELL = (16, 32)
# Unmounting the initramfs from /system hides the fonts (the rescue programs go
# too, so this is the last thing the case does), and the game draws with the
# console font, mono-16.
HIDE_FONTS = "umount /system\n"
FONT_WARNING = re.compile(
    r"cannot load the font sans-48, using the console font"
)
HARD_DROP_ROUNDS = 12
HARD_DROPS_PER_ROUND = 5
INITIALS_GUARD_SECONDS = 1.2
SCORE_FILE = "/home/.featherfall"
SCORE_LINE = re.compile(r"^ABC (\d+)\r?$", re.MULTILINE)
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


def CellCenter(column, row):
    return WELL_X + CELL * column + CELL // 2, WELL_Y + CELL * row + CELL // 2


def ExpectFrame(pixels):
    for what, point, wanted in (
        ("the well border", (WELL_X - 2, WELL_Y + 100), WELL_BORDER),
        ("the left panel border", (LEFT_PANEL_X, WELL_Y + 100), PANEL_BORDER),
        ("the background", (10, 400), BACKGROUND),
        ("an empty cell", CellCenter(9, 10), WELL_FILL),
        ("a grid line", (WELL_X + 9 * CELL, WELL_Y + 10 * CELL), GRID),
    ):
        failure = ExpectColor(pixels, what, point, wanted)
        if failure is not None:
            return failure
    return None


def ExpectEmptyWell(pixels):
    failure = ExpectFrame(pixels)
    if failure is None:
        failure = ExpectColor(
            pixels,
            "the bottom row's second cell",
            CellCenter(1, 19),
            WELL_FILL,
        )
    return failure


# Every piece shape fills the second cell of its bottom row when it has been
# moved to the left wall and dropped.
def ExpectLockedPiece(pixels):
    failure = ExpectFrame(pixels)
    if failure is None:
        failure = ExpectNumbers(pixels)
    if failure is None:
        found = PixelAt(pixels, *CellCenter(1, 19))
        if found not in PIECE_COLORS:
            failure = (
                f"the bottom row's second cell {CellCenter(1, 19)} is "
                f"{Hex(found)}, expected a piece color"
            )
    return failure


@functools.cache
def NumberCells():
    """Every cell a number can show, rendered by ffnt.py from the committed
    mono-32 file as the game draws it: a digit or a space in TEXT over the
    panel's fill."""
    ffnt = LoadFfnt()
    font = FontFile("mono-32")
    cells = {}
    for symbol in "0123456789 ":
        width, height, pixels = ffnt.render_text(
            font, symbol, PANEL_FILL, TEXT, margin=0
        )
        if (width, height) != NUMBER_CELL:
            raise StepFailed(f"mono-32 {symbol!r} is {width}x{height}")
        cells[symbol] = tuple(pixels)
    return cells


def CellPixels(pixels, x, y):
    width, height = NUMBER_CELL
    return tuple(
        PixelAt(pixels, x + column, y + row)
        for row in range(height)
        for column in range(width)
    )


def NumberText(pixels, y):
    cells = NumberCells()
    text = ""
    for index in range(NUMBER_CELLS):
        found = CellPixels(pixels, NUMBER_X + index * NUMBER_CELL[0], y)
        symbol = next((s for s, cell in cells.items() if cell == found), None)
        if symbol is None:
            return None, index
        text += symbol
    return text, None


# A value drawn over its old one would leave pixels of both digits in a cell:
# every cell must be exactly a digit or a space, the digits first.
def ExpectNumbers(pixels):
    for name, y in zip(("score", "level", "lines"), NUMBER_ROWS):
        text, bad = NumberText(pixels, y)
        if text is None:
            return (
                f"cell {bad} of the {name} at y={y} is not a digit or a space"
            )
        if not re.fullmatch(r"\d+ *", text):
            return f"the {name} reads {text!r}, expected digits then spaces"
    return None


def ExpectPaused(pixels):
    failure = ExpectColor(
        pixels, "the pause box", (WELL_X + 8, WELL_Y + 8), OVERLAY_FILL
    )
    if failure is None and not Found(pixels, *PAUSED_BOX, ACCENT):
        failure = "the word PAUSED is not drawn"
    if failure is None:
        failure = ExpectColor(
            pixels, "a covered cell", CellCenter(1, 19), OVERLAY_FILL
        )
    return failure


def ExpectEntry(pixels):
    failure = ExpectColor(
        pixels, "the game-over box", (WELL_X + 8, WELL_Y + 8), OVERLAY_FILL
    )
    if failure is None:
        failure = ExpectColor(
            pixels,
            "the current initials slot",
            (WELL_X + SLOT_X, WELL_Y + 200),
            ACCENT,
        )
    if failure is None:
        failure = ExpectColor(
            pixels,
            "the next initials slot",
            (WELL_X + SLOT_X + SLOT_STEP, WELL_Y + 200),
            PANEL_BORDER,
        )
    return failure


def ExpectInitials(pixels):
    for slot in range(3):
        left = WELL_X + SLOT_X + SLOT_STEP * slot
        if not Found(
            pixels,
            left + 1,
            WELL_Y + 181,
            SLOT_SIZE[0] - 2,
            SLOT_SIZE[1] - 2,
            TEXT,
        ):
            return f"initials slot {slot} shows no letter"
    return None


def ExpectTable(pixels):
    failure = ExpectColor(
        pixels, "the table box", (WELL_X + 8, WELL_Y + 8), OVERLAY_FILL
    )
    if failure is None and not Found(pixels, *FIRST_INITIALS_BOX, ACCENT):
        failure = "the new entry is not highlighted in the first row"
    return failure


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


class Graphics(ScreenCase, Case):
    """Runs fbdemo on the first console and checks what QEMU's screendump
    shows while it owns the screen, on another console, after switching back,
    after drawing again and once it has released the screen; then runs
    fbdemo --keys and checks the key events QMP key presses produce; last it
    plays featherfall with QMP keys and checks the well, a locked piece, the
    pause box, the redraw after a console switch, the game-over screen, the
    initials entry, the high score table and the saved file; last it unmounts
    /system to hide the fonts and checks that it falls back to the console
    font."""

    serial_input = True
    timeout = GRAPHICS_TIMEOUT_SECONDS
    passed = (
        "fbdemo's pattern was drawn clipped without the status bar, the "
        "other console showed text, the blanked console stayed blank until "
        "it drew again, releasing brought the text console back, key "
        "events, switching, overflow and Ctrl+C behaved as specified, "
        "featherfall's self-test and demo runs gave the expected results, "
        "and featherfall drew its well, paused, redrew after a console "
        "switch, took initials for a high score, saved them and drew with the "
        "console font when its fonts were gone"
    )

    def __init__(self, args):
        super().__init__(args)
        self.InitScreen()
        self.state = 0
        self.finished = False

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
        self.RunGame(session, events)

    def RunGame(self, session, events):
        events.origin = len(events.Text())
        session.process.stdin.write(
            f"featherfall --seed {GAME_SEED}\n".encode()
        )
        session.process.stdin.flush()
        self.Step("featherfall's well", ExpectEmptyWell, GAME_START_SECONDS)

        self.Chord(("left", True))
        time.sleep(0.8)
        self.Chord(("left", False))
        self.Tap("spc")
        self.Step("a piece at the left wall", ExpectLockedPiece)

        self.Tap("up")
        self.Tap("z")
        self.Tap("c")
        self.Chord(("right", True))
        time.sleep(0.4)
        self.Chord(("right", False))
        self.Tap("spc")
        self.Tap("p")
        self.Step("the pause box", ExpectPaused)
        self.Tap("p")
        self.Step("after resuming", ExpectLockedPiece)

        self.Tap("p")
        self.Step("paused again", ExpectPaused)
        self.Chord(("alt", True), ("f2", True))
        self.Step("featherfall's console hidden", ExpectText)
        self.Chord(("f2", False), ("alt", False))
        self.Chord(("alt", True), ("f1", True))
        self.Chord(("f1", False), ("alt", False))
        self.Step("redrawn after switching back", ExpectPaused)
        self.Tap("p")
        self.Step("resumed after switching", ExpectLockedPiece)

        for _ in range(HARD_DROP_ROUNDS):
            for _ in range(HARD_DROPS_PER_ROUND):
                self.Tap("spc")
            time.sleep(0.3)
            if self.Probe(ExpectEntry):
                break
        self.Step("the game-over screen", ExpectEntry)
        time.sleep(INITIALS_GUARD_SECONDS)
        for key in ("a", "b", "c"):
            self.Tap(key)
        self.Step("the initials", ExpectInitials)
        self.Tap("ret")
        self.Step("the high score table", ExpectTable)
        self.Tap("esc")
        if events.WaitText(SHELL_PROMPT_TEXT) is None:
            raise StepFailed("the prompt did not return after Esc")
        self.Step("after featherfall", ExpectTextAgain)

        session.process.stdin.write(f"cat {SCORE_FILE}\n".encode())
        session.process.stdin.flush()
        found = events.WaitText(SCORE_LINE)
        if found is None:
            raise StepFailed(f"{SCORE_FILE} has no 'ABC <score>' line")
        if int(found.group(1)) < 1:
            raise StepFailed("the saved score is not positive")
        self.RunWithoutFonts(session, events)

    def ExpectFallbackFrame(self, pixels):
        failure = ExpectEmptyWell(pixels)
        if failure is None:
            failure = ExpectRenderedText(
                pixels,
                "mono-16",
                "FEATHERFALL",
                0,
                14,
                ACCENT,
                BACKGROUND,
                WIDTH,
            )
        return failure

    def RunWithoutFonts(self, session, events):
        events.origin = len(events.Text())
        session.process.stdin.write(HIDE_FONTS.encode())
        session.process.stdin.write(
            f"featherfall --seed {GAME_SEED}\n".encode()
        )
        session.process.stdin.flush()
        self.Step(
            "featherfall without fonts",
            self.ExpectFallbackFrame,
            GAME_START_SECONDS,
        )
        if events.WaitText(FONT_WARNING) is None:
            raise StepFailed("featherfall did not say it fell back")
        events.origin = len(events.Text())
        self.Tap("esc")
        if events.WaitText(SHELL_PROMPT_TEXT) is None:
            raise StepFailed("the prompt did not return after Esc")
        self.Step("after featherfall without fonts", ExpectTextAgain)

    def OnOutput(self, session, clean_output):
        if self.finished:
            return
        try:
            if self.state == 0 and clean_output.count(PRESENTED) >= 1:
                self.OpenScreen()
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
        self.CloseScreen()

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
