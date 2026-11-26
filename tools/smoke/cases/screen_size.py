import functools
import json
import re
import time

from smoke.boot import NETWORK_DHCP_MESSAGE, PANIC_MARKER
from smoke.cases.base import Case
from smoke.cases.fonts import CELL_HEIGHT, CELL_WIDTH, Reference
from smoke.cases.screen import BLACK, Difference, Hex, PixelAt, ScreenCase
from smoke.script import Lines, WaitFor, WaitUntil
from smoke.terminal import SHELL_PROMPT

SCREEN_SIZE_TIMEOUT_SECONDS = 90
DEMO_COMMAND = "fbdemo 30\n"
READY_COMMAND = "echo ready\n"
PRESENTED = b"fbdemo: presented"
DONE = b"fbdemo: done"
READY_OUTPUT = re.compile(rb"^ready\r?$", re.MULTILINE)
DISPLAY_LINE = re.compile(r"KERN/I: Display: (\d+)x(\d+), console (\d+)x(\d+)")
TTY_LINE = re.compile(r"^fbdemo: tty1 (\d+)x(\d+)\r?$", re.MULTILINE)
STATUS_LEFT = "tty1  shell"
# One cell of padding all round; row 0 of the grid is the status bar.
PADDING_CELLS = 1
# The console's grid is at most this many cells.
MAX_COLUMNS = 320
MAX_ROWS = 100
BARS = (
    0xFF0000,
    0x00FF00,
    0x0000FF,
    0xFFFFFF,
    0xFFFF00,
    0x00FFFF,
    0xFF00FF,
    0x808080,
)
SETTLE_SECONDS = 1.5


def GridSize(width, height):
    """The console's columns and rows (the status bar's row included) that
    Term::Init works out for a screen."""
    columns = min(width // CELL_WIDTH, MAX_COLUMNS) - 2 * PADDING_CELLS
    rows = min(height // CELL_HEIGHT, MAX_ROWS) - 2 * PADDING_CELLS
    return columns, rows


def PatternColor(x, y, width, height):
    """src/user/fbdemo.cpp's pattern: eight color bars over a gradient whose
    channels wrap at 256, a square clipped at the top-left corner and a patch
    that starts 24 pixels from the right and 68 from the bottom."""
    if -10 <= x < 10 and -10 <= y < 10:
        return 0x654321
    if x >= width - 24 and y >= height - 68:
        return 0x123456
    if y < height // 2:
        return BARS[min(x // (width // 8), 7)]
    return (((x // 4) & 0xFF) << 16) | ((((y - height // 2) // 2) & 0xFF) << 8)


@functools.cache
def PatternBytes(width, height):
    rows = []
    for y in range(height):
        row = bytearray()
        for x in range(width):
            color = PatternColor(x, y, width, height)
            row += bytes((color >> 16, (color >> 8) & 0xFF, color & 0xFF))
        rows.append(bytes(row))
    return b"".join(rows)


class ScreenSize(ScreenCase, Case):
    """Boots at the SCREEN size and checks, against QEMU's screendump, that
    the kernel logs that size and a console grid to fit, that the status bar
    and the shell prompt sit at their cells as the FONTS reference rendering
    of the console font draws them, that fbdemo draws its pattern over the
    whole screen, and that the console comes back when fbdemo exits."""

    any_screen = True
    serial_input = True
    timeout = SCREEN_SIZE_TIMEOUT_SECONDS

    def __init__(self, args):
        super().__init__(args)
        self.InitScreen()
        self.reference = Reference()
        self.columns, self.rows = GridSize(self.width, self.height)
        self.state = 0
        self.checked = False
        self.finished = False
        self.prompt = ""

    @property
    def passed(self):
        return (
            f"the screen was {self.width}x{self.height} with a "
            f"{self.columns}x{self.rows} console, the status bar and the "
            "prompt were at their cells as the reference rendering draws "
            "them, fbdemo's pattern covered the screen and the console came "
            "back after it"
        )

    def Script(self, session):
        return [
            WaitFor(NETWORK_DHCP_MESSAGE),
            *Lines(READY_COMMAND.encode()),
            WaitUntil(lambda: self.checked),
            *Lines(DEMO_COMMAND.encode()),
        ]

    def Markers(self):
        return (PANIC_MARKER.encode(),)

    def Finished(self, clean_output):
        return self.finished

    def CellRows(self, pixels, column, row):
        x = CELL_WIDTH * (column + PADDING_CELLS)
        y = CELL_HEIGHT * (row + PADDING_CELLS)
        rows = []
        for line in range(CELL_HEIGHT):
            start = 3 * ((y + line) * self.width + x)
            rows.append(pixels[start : start + 3 * CELL_WIDTH])
        return rows

    def CellIs(self, pixels, column, row, character, reverse=False):
        wanted = self.reference.Cell(ord(character), reverse)
        return self.CellRows(pixels, column, row) == wanted

    def TextRowDifference(self, pixels, row, text, reverse=False, skip=()):
        """The first column of a grid row whose cell differs from the text
        followed by blanks; a skipped column is the cursor's."""
        cells = text + " " * (self.columns - len(text))
        for column, character in enumerate(cells):
            if column in skip:
                continue
            if not self.CellIs(pixels, column, row, character, reverse):
                return column
        return None

    def FindRow(self, pixels, text, first=1, skip=()):
        for row in range(first, self.rows):
            if self.TextRowDifference(pixels, row, text, skip=skip) is None:
                return row
        return None

    def ExpectStatusBar(self, pixels):
        if self.rows < 2 or self.columns < len(STATUS_LEFT) + 2:
            return f"a {self.columns}x{self.rows} grid has no room"
        if not self.CellIs(pixels, 0, 0, " ", reverse=True):
            return "the status bar's first cell is not blank on white"
        for index, character in enumerate(STATUS_LEFT):
            if not self.CellIs(pixels, 1 + index, 0, character, True):
                return (
                    f"the status bar's {character!r} at column {1 + index} "
                    "differs from the reference"
                )
        if not self.CellIs(pixels, self.columns - 1, 0, " ", reverse=True):
            return (
                f"the status bar's last cell (column {self.columns - 1}) is "
                "not blank on white"
            )
        return self.ExpectPadding(pixels)

    def ExpectPadding(self, pixels):
        right = CELL_WIDTH * (self.columns + PADDING_CELLS)
        bottom = CELL_HEIGHT * (self.rows + PADDING_CELLS)
        points = (
            ("the top-left corner", 0, 0),
            ("the top-right corner", self.width - 1, 0),
            ("the left padding of the status bar", 0, CELL_HEIGHT + 4),
            ("the right padding of the status bar", right, CELL_HEIGHT + 4),
            (
                "the right edge of the status bar",
                self.width - 1,
                CELL_HEIGHT + 4,
            ),
            ("the bottom-left corner", 0, self.height - 1),
            ("the bottom-right corner", self.width - 1, self.height - 1),
            ("below the last row", CELL_WIDTH * 4, bottom),
        )
        for what, x, y in points:
            if y >= self.height or x >= self.width:
                continue
            found = PixelAt(pixels, x, y, self.width)
            if found != BLACK:
                return f"{what} at ({x},{y}) is {Hex(found)}, expected 000000"
        return None

    def ExpectPrompt(self, after_text):
        """The prompt on the row below the line of after_text, as typed text
        and blanks, but for the cursor's cell."""

        def Check(pixels):
            failure = self.ExpectStatusBar(pixels)
            if failure is not None:
                return failure
            row = self.FindRow(pixels, after_text)
            if row is None:
                return f"the line {after_text!r} is nowhere on the screen"
            if row + 1 >= self.rows:
                return f"the line {after_text!r} is on the last row"
            cursor = len(self.prompt)
            column = self.TextRowDifference(
                pixels, row + 1, self.prompt, skip=(cursor,)
            )
            if column is not None:
                return (
                    f"the prompt row {row + 1} differs from {self.prompt!r} "
                    f"at column {column}"
                )
            return None

        return Check

    def ExpectPattern(self, pixels):
        return Difference(
            pixels, PatternBytes(self.width, self.height), self.width
        )

    def Screen(self, name, check):
        try:
            self.Step(name, check)
        except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as e:
            if self.failure is None:
                self.failure = f"{name}: screendump failed: {e}"

    def OnOutput(self, session, clean_output):
        if self.finished:
            return
        text = clean_output
        if self.state == 0:
            ready = READY_OUTPUT.search(text, text.find(b"echo ready") + 1)
            if ready is None:
                return
            prompt = SHELL_PROMPT.search(text, ready.end())
            if prompt is None:
                return
            self.prompt = prompt.group().decode()
            self.state = 1
            try:
                self.OpenScreen()
            except OSError as e:
                self.failure = f"QMP failed: {e}"
            if self.failure is None:
                self.Screen("the shell", self.ExpectPrompt("ready"))
            self.checked = True
            self.finished = self.failure is not None
        elif self.state == 1 and text.count(PRESENTED) >= 1:
            self.state = 2
            time.sleep(SETTLE_SECONDS)
            self.Screen("fbdemo's pattern", self.ExpectPattern)
            if self.failure is not None:
                self.finished = True
                return
            session.process.stdin.write(b"q\n")
            session.process.stdin.flush()
        elif self.state == 2:
            done = text.find(DONE)
            if done >= 0 and SHELL_PROMPT.search(text, done):
                self.state = 3
                self.Screen("after fbdemo", self.ExpectPrompt("fbdemo: done"))
                self.finished = True

    def Stop(self):
        self.CloseScreen()

    def Check(self, result):
        if PANIC_MARKER in result.output:
            return "the kernel panicked"
        display = DISPLAY_LINE.search(result.output)
        if display is None:
            return f"the kernel logged no Display line{result.limit_note}"
        size = tuple(int(group) for group in display.groups())
        if size[:2] != (self.width, self.height):
            return (
                f"the display is {size[0]}x{size[1]}, not the requested "
                f"{self.width}x{self.height}"
            )
        if size[2:] != (self.columns, self.rows):
            return (
                f"the console is {size[2]}x{size[3]}, expected "
                f"{self.columns}x{self.rows}"
            )
        if self.failure is not None:
            return self.failure
        tty = TTY_LINE.search(result.output)
        if tty is None or tuple(map(int, tty.groups())) != size[:2]:
            return (
                f"fbdemo did not print 'tty1 {self.width}x{self.height}'"
                f"{result.limit_note}"
            )
        if not self.finished:
            return f"the session ended early{result.limit_note}"
        return None
