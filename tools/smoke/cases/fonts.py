import functools
import importlib.util
import json
import re
import sys
from pathlib import Path

from smoke.boot import PANIC_MARKER
from smoke.cases.base import Case
from smoke.cases.screen import (
    HEIGHT,
    WIDTH,
    Difference,
    Hex,
    PixelAt,
    ScreenCase,
    StepFailed,
)
from smoke.script import Lines, WaitUntil
from smoke.terminal import SHELL_PROMPT

FONTS_TIMEOUT_SECONDS = 120
TOOLS = Path(__file__).resolve().parents[2]
SAMPLE = TOOLS / "fonts" / "sample.txt"
CONSOLE_FONT = TOOLS.parent / "initramfs" / "fonts" / "mono-16.ffnt"
SAMPLE_COMMAND = "cat /system/fonts/sample.txt\n"
END_LINE = b"END OF SAMPLE"
# Typed on the serial line, which is the only input path that carries bytes
# above 0x7F: the QMP keyboard sends scancodes for ASCII keys alone.
ECHO_TEXT = "café ñandú ΑΩ жэ ┌─┐"
ECHO_COMMAND = f"echo {ECHO_TEXT}\n"
REJECTED_LINE = "fontview: font rejected length"
UNREADABLE_LINE = "fontview: font unreadable No such file or directory"
ECHO_OUTPUT = re.compile(
    rb"^" + re.escape(ECHO_TEXT.encode()) + rb"\r?$", re.MULTILINE
)
TYPED_PROMPT = re.compile(rb"^(/[^\n>]*> )echo ", re.MULTILINE)
BLACK = 0x000000
WHITE = 0xFFFFFF
FONT_DIRECTORY = TOOLS.parent / "initramfs" / "fonts"
FONT_NAMES = sorted(path.stem for path in FONT_DIRECTORY.glob("*.ffnt"))
BAD_FONT = "/tmp/bad.ffnt"
CARD_LINE = b"fontview: card drawn"
# src/user/fontview.cpp's card: the ten sizes' sample line and a missing box,
# stacked in the top half (light on dark) and again in the bottom half (dark
# on light), the pen at x 16 and each line 4 pixels below the one before.
CARD_FONTS = [
    "mono-16",
    "mono-20",
    "mono-24",
    "mono-32",
    "sans-16",
    "sans-20",
    "sans-24",
    "sans-32",
    "sans-48",
    "sans-64",
]
CARD_SAMPLE = "Hamburgefonstiv 0123 é"
CARD_MISSING = ("sans-24", "Missing: \u0378")
CARD_HALVES = ((0, 0x101820, 0xE8E8E8), (384, 0xF4F0E8, 0x182030))
CARD_MARGIN = 16
CARD_TOP = 8
CARD_GAP = 4
CHECK_COMMANDS = [
    f"fontview --check /system/fonts/{name}.ffnt\n" for name in FONT_NAMES
] + [
    f"cp /system/fonts/sans-16.ffnt {BAD_FONT}\n",
    f"echo x >> {BAD_FONT}\n",
    f"fontview --check {BAD_FONT}\n",
    "fontview --check /nonexistent.ffnt\n",
    "fontview --card\n",
]
# The console's 8x16 cells with one cell of padding all round; row 0 is the
# status bar.
CELL_WIDTH = 8
CELL_HEIGHT = 16
COLUMNS = WIDTH // CELL_WIDTH - 2
ROWS = HEIGHT // CELL_HEIGHT - 2
REPLACEMENT = 0xFFFD
# [\xff] [\xc0\x80] [\xe2\x82] [\xed\xa0\x80] [\xf4\x90\x80\x80] [\x80\xbf]
# [\xf0\x9f\x98]: one U+FFFD per maximal subpart.
REPLACEMENT_CELLS = 14


@functools.cache
def LoadFfnt():
    spec = importlib.util.spec_from_file_location(
        "ffnt", TOOLS / "fonts" / "ffnt.py"
    )
    module = importlib.util.module_from_spec(spec)
    sys.modules["ffnt"] = module
    spec.loader.exec_module(module)
    return module


@functools.cache
def FontFile(name):
    return LoadFfnt().read(
        (TOOLS.parent / "initramfs" / "fonts" / f"{name}.ffnt").read_bytes()
    )


def ExpectRenderedText(
    pixels, font_name, text, x, y, foreground, background, width=None
):
    """The text a program drew with the committed font, compared pixel by
    pixel with ffnt.py's rendering, plus a margin of background around it. x
    and y are the pen and the top of the line box; with a width the text is
    centered in [x, x + width)."""
    margin = 4
    ffnt = LoadFfnt()
    font = FontFile(font_name)
    left = x
    if width is not None:
        left = x + (width - ffnt.measure(font, text)) // 2
    box_width, box_height, wanted = ffnt.render_text(
        font, text, background, foreground, margin
    )
    for row in range(box_height):
        for column in range(box_width):
            px = left - margin + column
            py = y - margin + row
            found = PixelAt(pixels, px, py)
            expected = wanted[row * box_width + column]
            if found != expected:
                return (
                    f"{text!r} in {font_name} differs at ({px},{py}): "
                    f"{Hex(found)}, expected {Hex(expected)}"
                )
    return None


def CardReference():
    """The whole card as the 1024x768 RGB bytes ffnt.py renders from the
    committed files, laid out by the rules above and independent of
    fontview.cpp."""
    ffnt = LoadFfnt()
    screen = bytearray()
    for top, background, text in CARD_HALVES:
        half = [background] * (WIDTH * 384)
        lines = [(name, CARD_SAMPLE) for name in CARD_FONTS] + [CARD_MISSING]
        y = top + CARD_TOP
        for name, string in lines:
            font = FontFile(name)
            if CARD_MARGIN + ffnt.measure(font, string) > WIDTH:
                raise RuntimeError(f"the card's {name} line is too wide")
            box_width, box_height, box = ffnt.render_text(
                font, string, background, text, 4
            )
            for row in range(box_height):
                for column in range(box_width):
                    pixel = box[row * box_width + column]
                    if pixel == background:
                        continue
                    px = CARD_MARGIN - 4 + column
                    py = y - 4 + row - top
                    if 0 <= px < WIDTH and 0 <= py < 384:
                        half[py * WIDTH + px] = pixel
            y += font.line_height + CARD_GAP
        if y - CARD_GAP > top + 384:
            raise RuntimeError("the card's lines do not fit their half")
        for pixel in half:
            screen += bytes((pixel >> 16, (pixel >> 8) & 255, pixel & 255))
    return bytes(screen)


def IsC1(code_point):
    return 0x80 <= code_point <= 0x9F


class Reference:
    """The sample's rows as cells rendered by ffnt.py from the committed
    mono-16 file: each cell is the glyph clipped to 8x16, blended over black
    as the console does."""

    def __init__(self):
        self.ffnt = LoadFfnt()
        self.font = self.ffnt.read(CONSOLE_FONT.read_bytes())
        self.glyphs = {glyph.code_point: glyph for glyph in self.font.glyphs}
        self.cells = {}
        data = SAMPLE.read_bytes()
        self.lines = [
            [
                ord(character)
                for character in line.decode("utf-8", errors="replace")
                if not IsC1(ord(character))
            ]
            for line in data.split(b"\n")[:-1]
        ]
        self.replacements = sum(line.count(REPLACEMENT) for line in self.lines)

    def Cell(self, code_point):
        if code_point in self.cells:
            return self.cells[code_point]
        font = self.font
        glyph = self.glyphs.get(code_point, self.glyphs[0])
        pixels = [[BLACK] * CELL_WIDTH for _ in range(CELL_HEIGHT)]
        if code_point > 0x20:
            for y, row in enumerate(glyph.rows):
                py = font.ascent - glyph.top + y
                for x, coverage in enumerate(row):
                    px = glyph.left + x
                    if (
                        coverage
                        and 0 <= px < CELL_WIDTH
                        and 0 <= py < CELL_HEIGHT
                    ):
                        pixels[py][px] = self.ffnt.blend(
                            BLACK, WHITE, coverage
                        )
        rows = [
            b"".join(bytes((c >> 16, (c >> 8) & 255, c & 255)) for c in row)
            for row in pixels
        ]
        self.cells[code_point] = rows
        return rows

    def Render(self, text):
        cells = [ord(character) for character in text]
        cells += [0x20] * (COLUMNS - len(cells))
        return [
            b"".join(self.Cell(cell)[y] for cell in cells)
            for y in range(CELL_HEIGHT)
        ]

    def Row(self, index):
        cells = self.lines[index] + [0x20] * (COLUMNS - len(self.lines[index]))
        return [
            b"".join(self.Cell(cell)[y] for cell in cells)
            for y in range(CELL_HEIGHT)
        ]

    def Contains(self, code_point):
        return code_point in self.glyphs


def ScreenRow(pixels, row, y):
    start = 3 * ((row * CELL_HEIGHT + CELL_HEIGHT + y) * WIDTH + CELL_WIDTH)
    return pixels[start : start + 3 * COLUMNS * CELL_WIDTH]


def FirstDifference(pixels, row, expected):
    for y in range(CELL_HEIGHT):
        found = ScreenRow(pixels, row, y)
        if found != expected[y]:
            width = 3 * CELL_WIDTH
            for column in range(COLUMNS):
                part = slice(column * width, (column + 1) * width)
                if found[part] != expected[y][part]:
                    return column, y
    return None


class Fonts(ScreenCase, Case):
    """Prints a sample of UTF-8 text with `cat` and matches the screen, cell
    by cell, against the cells ffnt.py renders from the committed console
    font: accented Latin, Greek, Cyrillic, arrows, box drawing, blocks, a
    dropped C1 control and invalid bytes that must show as U+FFFD. Then types
    `echo` with accented text and box drawing on the serial line, and matches
    the line editor's echo and the program's output on screen and in the
    session."""

    serial_input = True
    timeout = FONTS_TIMEOUT_SECONDS
    passed = (
        "the console drew the UTF-8 sample, rows of accented Latin, Greek, "
        "Cyrillic, arrows, box drawing, blocks and U+FFFD cells for invalid "
        "bytes, exactly as the reference rendering of mono-16 draws them; "
        "typed accented text and box drawing were echoed by the line editor "
        "and printed by echo, on the screen and over serial; fontview "
        "accepted every font file and refused one with a byte appended, "
        "and its test card matched the reference rendering exactly"
    )

    def __init__(self, args):
        super().__init__(args)
        self.InitScreen()
        self.reference = Reference()
        self.state = 0
        self.sampled = False
        self.echoed = False
        self.carded = False
        self.finished = False
        self.card = None

    def Script(self, session):
        return (
            Lines(SAMPLE_COMMAND.encode())
            + [
                WaitUntil(lambda: self.sampled),
                ECHO_COMMAND.encode(),
                WaitUntil(lambda: self.echoed),
            ]
            + [command.encode() for command in CHECK_COMMANDS]
        )

    def Markers(self):
        return (PANIC_MARKER.encode(),)

    def Finished(self, clean_output):
        return self.finished

    def ExpectSample(self, pixels):
        reference = self.reference
        count = len(reference.lines)
        for first in range(1, ROWS - count + 1):
            if FirstDifference(pixels, first, reference.Row(0)) is not None:
                continue
            for index in range(1, count):
                expected = reference.Row(index)
                difference = FirstDifference(pixels, first + index, expected)
                if difference is not None:
                    column, y = difference
                    code_point = (
                        reference.lines[index][column]
                        if column < len(reference.lines[index])
                        else 0x20
                    )
                    return (
                        f"sample line {index} differs from the reference at "
                        f"column {column}, pixel row {y} (U+{code_point:04X})"
                    )
            return None
        return "the sample's first line is nowhere on the screen"

    def ExpectEcho(self, prompt):
        typed = self.reference.Render(prompt + "echo " + ECHO_TEXT)
        printed = self.reference.Render(ECHO_TEXT)

        def Check(pixels):
            for first in range(1, ROWS - 1):
                if FirstDifference(pixels, first, typed) is not None:
                    continue
                difference = FirstDifference(pixels, first + 1, printed)
                if difference is None:
                    return None
                return (
                    f"the echoed line differs from the reference at column "
                    f"{difference[0]}, pixel row {difference[1]}"
                )
            return "the typed line is nowhere on the screen"

        return Check

    def OnOutput(self, session, clean_output):
        if self.state == 0:
            marker = clean_output.rfind(END_LINE)
            if marker < 0 or SHELL_PROMPT.search(clean_output, marker) is None:
                return
            self.state = 1
            try:
                self.OpenScreen()
                self.Step("the UTF-8 sample", self.ExpectSample)
            except StepFailed as failure:
                if self.failure is None:
                    self.failure = str(failure)
            except (
                OSError,
                RuntimeError,
                ValueError,
                json.JSONDecodeError,
            ) as e:
                self.failure = f"screendump failed: {e}"
            self.sampled = True
            if self.failure is not None:
                self.finished = True
            return
        if self.state >= 2:
            self.OnCard(clean_output)
            return
        if self.state != 1:
            return
        printed = ECHO_OUTPUT.search(clean_output)
        typed = TYPED_PROMPT.search(clean_output)
        if (
            printed is None
            or typed is None
            or SHELL_PROMPT.search(clean_output, printed.end()) is None
        ):
            return
        self.state = 2
        try:
            self.Step(
                "the typed text", self.ExpectEcho(typed.group(1).decode())
            )
        except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as e:
            self.failure = f"screendump failed: {e}"
        self.echoed = True
        if self.failure is not None:
            self.finished = True

    def ExpectCard(self, pixels):
        if self.card is None:
            self.card = CardReference()
        return Difference(pixels, self.card)

    def OnCard(self, clean_output):
        if self.state == 2:
            if CARD_LINE not in clean_output:
                return
            self.state = 3
            try:
                self.Step("the test card", self.ExpectCard)
                self.Tap("esc")
            except (
                OSError,
                RuntimeError,
                ValueError,
                json.JSONDecodeError,
            ) as e:
                self.failure = f"card check failed: {e}"
                self.finished = True
            return
        marker = clean_output.rfind(CARD_LINE)
        if SHELL_PROMPT.search(clean_output, marker) is not None:
            self.finished = True

    def Stop(self):
        self.CloseScreen()

    def Check(self, result):
        if PANIC_MARKER in result.output:
            return "the kernel panicked"
        reference = self.reference
        if reference.replacements != REPLACEMENT_CELLS:
            return (
                f"the reference decoder found {reference.replacements} "
                f"invalid-byte cells, expected {REPLACEMENT_CELLS}"
            )
        lacking = sorted(
            {
                code_point
                for line in reference.lines
                for code_point in line
                if code_point != REPLACEMENT
                and not reference.Contains(code_point)
            }
        )
        if lacking:
            return f"mono-16 lacks sample code points {lacking}"
        lacking = sorted(
            {
                character
                for character in ECHO_TEXT
                if character != " " and not reference.Contains(ord(character))
            }
        )
        if lacking:
            return f"mono-16 lacks echo text characters {lacking}"
        if self.failure is not None:
            return self.failure
        lines = result.output.replace("\r", "").split("\n")
        for name in FONT_NAMES:
            font = FontFile(name)
            expected = (
                f"fontview: font ok glyphs {len(font.glyphs)} "
                f"line {font.line_height}"
            )
            if expected not in lines:
                reported = [line for line in lines if "fontview:" in line]
                return (
                    f"fontview --check of {name}.ffnt gave no line "
                    f"{expected!r}: {reported}{result.limit_note}"
                )
        for expected in (REJECTED_LINE, UNREADABLE_LINE):
            if expected not in lines:
                reported = [line for line in lines if "fontview:" in line]
                return (
                    f"fontview --check gave no line {expected!r}: "
                    f"{reported}{result.limit_note}"
                )
        if not self.finished:
            return f"the session ended early{result.limit_note}"
        if f"echo {ECHO_TEXT}" not in result.output:
            return "the line editor did not echo the typed text over serial"
        if ECHO_OUTPUT.search(result.captured) is None:
            return "echo did not print the typed text over serial"
        return None
