import importlib.util
import json
import sys
from pathlib import Path

from smoke.boot import PANIC_MARKER
from smoke.cases.base import Case
from smoke.cases.screen import HEIGHT, WIDTH, ScreenCase, StepFailed
from smoke.script import Lines
from smoke.terminal import SHELL_PROMPT

FONTS_TIMEOUT_SECONDS = 120
TOOLS = Path(__file__).resolve().parents[2]
SAMPLE = TOOLS / "fonts" / "sample.txt"
CONSOLE_FONT = TOOLS.parent / "initramfs" / "fonts" / "mono-16.ffnt"
SAMPLE_COMMAND = "cat /system/fonts/sample.txt\n"
END_LINE = b"END OF SAMPLE"
BLACK = 0x000000
WHITE = 0xFFFFFF
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


def LoadFfnt():
    spec = importlib.util.spec_from_file_location(
        "ffnt", TOOLS / "fonts" / "ffnt.py"
    )
    module = importlib.util.module_from_spec(spec)
    sys.modules["ffnt"] = module
    spec.loader.exec_module(module)
    return module


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
    dropped C1 control and invalid bytes that must show as U+FFFD."""

    serial_input = True
    timeout = FONTS_TIMEOUT_SECONDS
    passed = (
        "the console drew the UTF-8 sample, rows of accented Latin, Greek, "
        "Cyrillic, arrows, box drawing, blocks and U+FFFD cells for invalid "
        "bytes, exactly as the reference rendering of mono-16 draws them"
    )

    def __init__(self, args):
        super().__init__(args)
        self.InitScreen()
        self.reference = Reference()
        self.state = 0
        self.finished = False

    def Script(self, session):
        return Lines(SAMPLE_COMMAND.encode())

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

    def OnOutput(self, session, clean_output):
        if self.state != 0:
            return
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
        except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as e:
            self.failure = f"screendump failed: {e}"
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
        if self.failure is not None:
            return self.failure
        if not self.finished:
            return f"the session ended early{result.limit_note}"
        return None
