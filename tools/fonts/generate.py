#!/usr/bin/env python3
"""Pre-renders the FFNT fonts from the pinned TrueType sources.

Needs freetype-py (tools/fonts/requirements.txt, in a virtual environment
such as tools/fonts/.venv); the build and the tests read the committed
output and do not run this.

  generate.py            write the outputs
  generate.py --check    regenerate into a temporary folder and compare
"""

from __future__ import annotations

import argparse
import hashlib
import math
import os
import sys
import tempfile
import textwrap
import unicodedata
from dataclasses import dataclass
from pathlib import Path

import ffnt

SKIP = 77

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
SOURCES = HERE / "sources"
FONTS = ROOT / "initramfs" / "fonts"
CONSOLE_C = ROOT / "src/kernel/framebuffer/console_font.c"
CONSOLE_H = ROOT / "src/kernel/framebuffer/console_font.h"
REFERENCE = ROOT / "tests/unit/user/data/font_reference.txt"

CONSOLE_FAMILY = "mono"
CONSOLE_SIZE = 16
CONSOLE_CELL = (8, 16)

REFERENCE_BACKGROUND = 0x203050
REFERENCE_FOREGROUND = 0xF0E0C0
REFERENCE_TEXTS = (
    "Hamburg 0123456789 ~!@#$%^&*()",
    "Árvíztűrő tükörfúrógép ŁÓDŹ čšž",
    "Ελληνικά Αλφάβητο ά ώ",
    "Привет мир ЖЁЯ",
    "┌─┬─┐│▀▄█░▒▓╔═╗╱╲╳",
    "←↑→↓↵ ■▲● … – — € ™ ͸ 中",
    "Bad \ufffd byte \ufffd\ufffd end",
)


@dataclass(frozen=True)
class Family:
    name: str
    display: str
    ttf: str
    licence: str
    licence_output: str
    monospace: bool
    sizes: tuple[int, ...]


FAMILIES = (
    Family(
        "mono",
        "JetBrains Mono",
        "JetBrainsMono-Regular.ttf",
        "OFL-JetBrainsMono.txt",
        "LICENSE-JetBrainsMono.txt",
        True,
        (16, 20, 24, 32),
    ),
    Family(
        "sans",
        "Inter",
        "Inter-Regular.ttf",
        "OFL-Inter.txt",
        "LICENSE-Inter.txt",
        False,
        (16, 20, 24, 32, 48, 64),
    ),
)

PUNCTUATION = (
    0x2013,
    0x2014,
    0x2018,
    0x2019,
    0x201A,
    0x201C,
    0x201D,
    0x201E,
    0x2020,
    0x2021,
    0x2022,
    0x2026,
    0x2030,
    0x2039,
    0x203A,
    0x20AC,
    0x2122,
)
SHAPES = (
    0x25A0,
    0x25A1,
    0x25B2,
    0x25B6,
    0x25BC,
    0x25C0,
    0x25C6,
    0x25CB,
    0x25CF,
)
SYNTHESISED = range(0x2500, 0x25A0)
REQUESTED = sorted(
    {
        *range(0x20, 0x7F),
        *range(0xA0, 0x180),
        *PUNCTUATION,
        *range(0x2190, 0x219A),
        0x21B5,
        0x21E5,
        0x21E7,
        *SYNTHESISED,
        *SHAPES,
        0xFFFD,
        *range(0x370, 0x400),
        *range(0x400, 0x460),
        0x490,
        0x491,
    }
)
ASCII = range(0x20, 0x7F)
ACCENT_PROBES = "ÁÉÍÓÚÅŐŰ"


def die(message: str) -> None:
    sys.exit(f"generate.py: {message}")


def read_sources() -> dict[str, tuple[str, str]]:
    entries = {}
    text = (SOURCES / "sources.txt").read_text(encoding="utf-8")
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        path, digest, version, _ = line.split("\t")
        actual = hashlib.sha256((SOURCES / path).read_bytes()).hexdigest()
        if actual != digest:
            die(f"{path} does not match its SHA-256 in sources.txt")
        entries[path] = (digest, version)
    return entries


def quantise(value: int) -> int:
    return (value * 15 + 127) // 255


def crop(
    rows: list[bytes], left: int, top: int
) -> tuple[list[bytes], int, int]:
    used = [y for y, row in enumerate(rows) if any(row)]
    if not used:
        return [], left, top
    first, last = used[0], used[-1] + 1
    columns = [x for row in rows[first:last] for x, v in enumerate(row) if v]
    x0, x1 = min(columns), max(columns) + 1
    return ([row[x0:x1] for row in rows[first:last]], left + x0, top - first)


class Source:
    def __init__(self, path: Path) -> None:
        import freetype

        self.freetype = freetype
        self.face = freetype.Face(str(path))

    def set_size(self, em: float) -> None:
        self.face.set_char_size(int(round(em * 64)), 0, 72, 72)

    def has(self, code_point: int) -> bool:
        return self.face.get_char_index(code_point) != 0

    def render(self, code_point: int) -> ffnt.Glyph:
        freetype = self.freetype
        index = self.face.get_char_index(code_point)
        self.face.load_glyph(
            index, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_LIGHT
        )
        slot = self.face.glyph
        bitmap = slot.bitmap
        rows = []
        for y in range(bitmap.rows):
            start = y * bitmap.pitch
            rows.append(
                bytes(
                    quantise(v)
                    for v in bitmap.buffer[start : start + bitmap.width]
                )
            )
        rows, left, top = crop(rows, slot.bitmap_left, slot.bitmap_top)
        return ffnt.Glyph(
            code_point, left, top, (slot.advance.x + 32) >> 6, rows
        )


STYLE_WORDS = {"LIGHT": 1, "HEAVY": 2, "DOUBLE": 3, "SINGLE": 1}
DIRECTIONS = {
    "UP": ("up",),
    "DOWN": ("down",),
    "LEFT": ("left",),
    "RIGHT": ("right",),
    "VERTICAL": ("up", "down"),
    "HORIZONTAL": ("left", "right"),
}
DASH_COUNTS = {"DOUBLE": 2, "TRIPLE": 3, "QUADRUPLE": 4}


def box_kind(code_point: int):
    name = unicodedata.name(chr(code_point))
    words = name.split()[2:]
    if "DASH" in words:
        return (
            "dash",
            STYLE_WORDS[words[0]],
            DASH_COUNTS[words[1]],
            words[-1] == "HORIZONTAL",
        )
    if "ARC" in words:
        directions = [
            d
            for w in words[words.index("ARC") + 1 :]
            if w in DIRECTIONS
            for d in DIRECTIONS[w]
        ]
        vertical = next(d for d in directions if d in ("up", "down"))
        horizontal = next(d for d in directions if d in ("left", "right"))
        return ("arc", vertical, horizontal)
    if "DIAGONAL" in words:
        if "CROSS" in words:
            return ("diagonal", True, True)
        return (
            "diagonal",
            "RIGHT TO" in " ".join(words),
            "LEFT TO" in " ".join(words),
        )
    arms = {}
    style = None
    group: list[str] = []
    groups = []
    for word in words + ["AND"]:
        if word == "AND":
            groups.append(group)
            group = []
        else:
            group.append(word)
    for group in groups:
        style = next(
            (STYLE_WORDS[w] for w in group if w in STYLE_WORDS), style
        )
        for word in group:
            for direction in DIRECTIONS.get(word, ()):
                arms[direction] = style
    if not arms:
        die(f"cannot read the box drawing name {name}")
    return ("arms", arms)


class Cell:
    def __init__(self, width: int, height: int) -> None:
        self.width = width
        self.height = height
        self.rows = [bytearray(width) for _ in range(height)]

    def fill(
        self, x0: int, y0: int, x1: int, y1: int, value: int = 15
    ) -> None:
        for y in range(max(0, y0), min(self.height, y1)):
            row = self.rows[y]
            for x in range(max(0, x0), min(self.width, x1)):
                row[x] = max(row[x], value)

    def sample(self, predicate, n: int = 8) -> None:
        for y in range(self.height):
            for x in range(self.width):
                hits = 0
                for j in range(n):
                    sy = y + (j + 0.5) / n
                    for i in range(n):
                        if predicate(x + (i + 0.5) / n, sy):
                            hits += 1
                value = (hits * 15 + n * n // 2) // (n * n)
                self.rows[y][x] = max(self.rows[y][x], value)

    def result(self) -> list[bytes]:
        return [bytes(row) for row in self.rows]


def strokes(style: int, size: int, t: int) -> list[tuple[int, int]]:
    if style == 3:
        start = (size - 3 * t) // 2
        return [(start, start + t), (start + 2 * t, start + 3 * t)]
    thickness = t if style == 1 else 2 * t
    start = (size - thickness) // 2
    return [(start, start + thickness)]


def arm_end(direction, index, arms, geometry, size_along) -> int:
    """Where stroke `index` of an arm stops on its way to the centre.

    A single stroke runs to the far edge of the perpendicular arms. Of a
    double arm, the stroke on the side of a perpendicular arm turns into
    that arm's stroke on this arm's side; the other one runs to the far
    edge of the arm on the opposite side. An arm with no perpendicular
    arm stops at the far edge of a centred band as thick as itself, so
    two opposite arms overlap.
    """
    horizontal = direction in ("left", "right")
    perpendicular = ("up", "down") if horizontal else ("left", "right")
    toward_low = direction in ("left", "up")
    own = geometry[direction]
    present = [p for p in perpendicular if p in arms]
    if not present:
        span = own[-1][1] - own[0][0]
        start = (size_along - span) // 2
        return start + span if toward_low else start
    if len(own) == 1:
        if toward_low:
            return max(geometry[p][-1][1] for p in present)
        return min(geometry[p][0][0] for p in present)
    side = perpendicular[index]
    if side in arms:
        reference = geometry[side]
        stroke = reference[0] if toward_low else reference[-1]
    else:
        reference = geometry[perpendicular[1 - index]]
        stroke = reference[-1] if toward_low else reference[0]
    return stroke[1] if toward_low else stroke[0]


def draw_arms(cell: Cell, arms, t: int) -> None:
    width, height = cell.width, cell.height
    geometry = {
        d: strokes(style, width if d in ("up", "down") else height, t)
        for d, style in arms.items()
    }
    for direction, spans in geometry.items():
        horizontal = direction in ("left", "right")
        size_along = width if horizontal else height
        for index, (lo, hi) in enumerate(spans):
            end = arm_end(direction, index, arms, geometry, size_along)
            if direction == "left":
                cell.fill(0, lo, end, hi)
            elif direction == "right":
                cell.fill(end, lo, width, hi)
            elif direction == "up":
                cell.fill(lo, 0, hi, end)
            else:
                cell.fill(lo, end, hi, height)


def draw_dash(cell: Cell, style: int, count: int, horizontal: bool, t: int):
    thickness = t if style == 1 else 2 * t
    along = cell.width if horizontal else cell.height
    across = cell.height if horizontal else cell.width
    start = (across - thickness) // 2
    for i in range(count):
        low = i * along // count
        high = (i + 1) * along // count
        gap = max(1, (high - low) // 3)
        low += gap // 2
        high = max(low + 1, high - (gap - gap // 2))
        if horizontal:
            cell.fill(low, start, high, start + thickness)
        else:
            cell.fill(start, low, start + thickness, high)


def draw_arc(cell: Cell, vertical: str, horizontal: str, t: int) -> None:
    cx, cy = cell.width / 2, cell.height / 2
    radius = min(cell.width, cell.height) / 2
    sign_h = 1 if horizontal == "right" else -1
    sign_v = 1 if vertical == "down" else -1
    ccx, ccy = cx + sign_h * radius, cy + sign_v * radius
    half = t / 2

    def inside(sx: float, sy: float) -> bool:
        toward_x = (sx - ccx) * sign_h
        toward_y = (sy - ccy) * sign_v
        if toward_x <= 0 and toward_y <= 0:
            return abs(math.hypot(sx - ccx, sy - ccy) - radius) <= half
        if toward_y >= 0 and abs(sx - cx) <= half:
            return toward_x <= 0
        if toward_x >= 0 and abs(sy - cy) <= half:
            return toward_y <= 0
        return False

    cell.sample(inside)


def draw_diagonal(cell: Cell, rising: bool, falling: bool, t: int) -> None:
    w, h = cell.width, cell.height
    norm = math.hypot(w, h)
    half = t / 2

    def inside(sx: float, sy: float) -> bool:
        if rising and abs(h * sx + w * sy - w * h) / norm <= half:
            return True
        return falling and abs(h * sx - w * sy) / norm <= half

    cell.sample(inside)


BLOCKS = {
    0x2580: [(0, 0, 8, 4)],
    0x2588: [(0, 0, 8, 8)],
    0x2590: [(4, 0, 8, 8)],
    0x2594: [(0, 0, 8, 1)],
    0x2595: [(7, 0, 8, 8)],
    0x2596: [(0, 4, 4, 8)],
    0x2597: [(4, 4, 8, 8)],
    0x2598: [(0, 0, 4, 4)],
    0x2599: [(0, 0, 4, 4), (0, 4, 4, 8), (4, 4, 8, 8)],
    0x259A: [(0, 0, 4, 4), (4, 4, 8, 8)],
    0x259B: [(0, 0, 4, 4), (4, 0, 8, 4), (0, 4, 4, 8)],
    0x259C: [(0, 0, 4, 4), (4, 0, 8, 4), (4, 4, 8, 8)],
    0x259D: [(4, 0, 8, 4)],
    0x259E: [(4, 0, 8, 4), (0, 4, 4, 8)],
    0x259F: [(4, 0, 8, 4), (0, 4, 4, 8), (4, 4, 8, 8)],
}
for _n in range(1, 8):
    BLOCKS[0x2580 + _n] = [(0, 8 - _n, 8, 8)]
    BLOCKS[0x2588 + _n] = [(0, 0, 8 - _n, 8)]
SHADES = {0x2591: 4, 0x2592: 8, 0x2593: 12}


def synthesise(code_point: int, width: int, height: int) -> list[bytes]:
    cell = Cell(width, height)
    t = max(1, width // 8)
    if code_point >= 0x2580:
        if code_point in SHADES:
            cell.fill(0, 0, width, height, SHADES[code_point])
        for fx0, fy0, fx1, fy1 in BLOCKS.get(code_point, []):
            cell.fill(
                (fx0 * width + 4) // 8,
                (fy0 * height + 4) // 8,
                (fx1 * width + 4) // 8,
                (fy1 * height + 4) // 8,
            )
        return cell.result()
    kind = box_kind(code_point)
    if kind[0] == "arms":
        draw_arms(cell, kind[1], t)
    elif kind[0] == "dash":
        draw_dash(cell, kind[1], kind[2], kind[3], t)
    elif kind[0] == "arc":
        draw_arc(cell, kind[1], kind[2], t)
    else:
        draw_diagonal(cell, kind[1], kind[2], t)
    return cell.result()


def missing_box(width: int, em: float) -> ffnt.Glyph:
    margin = max(1, width // 8)
    box_width = width - 2 * margin
    box_height = max(3, int(0.72 * em + 0.5))
    rows = []
    for y in range(box_height):
        if y in (0, box_height - 1):
            rows.append(bytes([15] * box_width))
        else:
            rows.append(bytes([15] + [0] * (box_width - 2) + [15]))
    return ffnt.Glyph(0, margin, box_height, width, rows)


def replacement_glyph(
    source: Source, em: float, ascent: int, line_height: int
) -> ffnt.Glyph:
    # A diamond over the whole line box, by 8x8 supersampling in integers, with
    # the source's own question mark knocked out of it.
    height = line_height
    width = height * 3 // 4
    margin = max(1, height // 16)
    inside = [[0] * width for _ in range(height)]
    for py in range(height):
        for px in range(width):
            hits = 0
            for j in range(8):
                for i in range(8):
                    x = 2 * (px * 8 + i) + 1
                    y = 2 * (py * 8 + j) + 1
                    if (
                        abs(x - 8 * width) * height
                        + abs(y - 8 * height) * width
                        <= 8 * width * height
                    ):
                        hits += 1
            inside[py][px] = (hits * 15 + 32) // 64
    source.set_size(em * 0.62)
    mark = source.render(ord("?"))
    source.set_size(em)
    mark_width = len(mark.rows[0])
    mark_height = len(mark.rows)
    left = (width - mark_width) // 2
    top = (height - mark_height) // 2
    for y, row in enumerate(mark.rows):
        for x, value in enumerate(row):
            cell = inside[top + y][left + x]
            inside[top + y][left + x] = (cell * (15 - value) + 7) // 15
    rows, glyph_left, glyph_top = crop(
        [bytes(row) for row in inside], margin, ascent
    )
    return ffnt.Glyph(0xFFFD, glyph_left, glyph_top, width + 2 * margin, rows)


def vertical_metrics(source: Source, line_height: int) -> tuple[int, int]:
    probes = [*ASCII, *(ord(c) for c in ACCENT_PROBES)]
    top = bottom = 0
    for code_point in probes:
        glyph = source.render(code_point)
        top = max(top, glyph.top)
        bottom = max(bottom, glyph.height - glyph.top)
    slack = line_height - top - bottom
    ascent = top + slack // 2 if slack > 0 else top
    ascent = min(ascent, line_height)
    return ascent, line_height - ascent


def ascii_fits(source: Source, width: int, line_height: int) -> bool:
    ascent, descent = vertical_metrics(source, line_height)
    for code_point in ASCII:
        glyph = source.render(code_point)
        if not glyph.rows:
            continue
        if (
            glyph.left < 0
            or glyph.left + glyph.width > width
            or glyph.top > ascent
            or glyph.height - glyph.top > descent
        ):
            return False
    return True


def console_em(source: Source) -> float:
    width, height = CONSOLE_CELL
    nominal = height * 5 / 6
    candidates = [nominal] + [
        step / 16 for step in range(int(nominal * 16), 8 * 16, -1)
    ]
    for em in candidates:
        source.set_size(em)
        if ascii_fits(source, width, height):
            return em
    die("no size makes the console font fit its cell")
    return 0.0


def clip_to_cell(
    glyph: ffnt.Glyph, ascent: int, width: int, height: int
) -> tuple[ffnt.Glyph, bool]:
    first_row = ascent - glyph.top
    rows = []
    for y, row in enumerate(glyph.rows):
        line = first_row + y
        if not 0 <= line < height:
            rows.append(bytes(len(row)))
            continue
        kept = bytearray(row)
        for x in range(len(row)):
            if not 0 <= glyph.left + x < width:
                kept[x] = 0
        rows.append(bytes(kept))
    clipped = any(a != b for a, b in zip(rows, glyph.rows))
    rows, left, top = crop(rows, glyph.left, glyph.top)
    return (
        ffnt.Glyph(glyph.code_point, left, top, glyph.advance, rows),
        clipped,
    )


@dataclass
class Built:
    font: ffnt.Font
    absent: list[int]
    clipped: list[int]
    em: float


def build(family: Family, source: Source, size: int, notice: str) -> Built:
    width = size // 2
    em = size * 5 / 6
    console = family.name == CONSOLE_FAMILY and size == CONSOLE_SIZE
    if console:
        em = console_em(source)
    source.set_size(em)
    ascent, descent = vertical_metrics(source, size)
    glyphs = [missing_box(width, em)]
    absent = []
    for code_point in REQUESTED:
        if code_point in SYNTHESISED:
            rows = synthesise(code_point, width, size)
            rows, left, top = crop(rows, 0, ascent)
            if not rows:
                die(f"U+{code_point:04X} came out empty")
            glyphs.append(ffnt.Glyph(code_point, left, top, width, rows))
        elif code_point == 0xFFFD and not family.monospace:
            glyphs.append(replacement_glyph(source, em, ascent, size))
        elif not source.has(code_point):
            absent.append(code_point)
        else:
            glyph = source.render(code_point)
            if family.monospace:
                glyph.advance = width
            glyphs.append(glyph)
    clipped = []
    if console:
        for index, glyph in enumerate(glyphs):
            glyphs[index], was_clipped = clip_to_cell(
                glyph, ascent, width, size
            )
            if was_clipped:
                clipped.append(glyph.code_point)
        if any(c in ASCII for c in clipped):
            die("the console font clips an ASCII glyph")
    font = ffnt.Font(
        f"{family.name} {size}",
        notice,
        family.monospace,
        int(round(em)),
        size,
        ascent,
        descent,
        width if family.monospace else 0,
        glyphs,
    )
    return Built(font, absent, clipped, em)


def notice_for(family: Family, versions: dict[str, tuple[str, str]]) -> str:
    licence = (SOURCES / family.licence).read_text(encoding="utf-8")
    copyright_line = licence.splitlines()[0].strip()
    version = versions[family.ttf][1]
    return (
        f"Derived from {family.display} Regular {version}. "
        f"{copyright_line}. "
        f"SIL Open Font License 1.1, see {family.licence_output}. "
        "Pre-rendered by tools/fonts/generate.py."
    )


def readme_text(versions: dict[str, tuple[str, str]]) -> str:
    lines = [
        "FeatherOS fonts",
        "",
        "The .ffnt files in this folder are bitmap fonts in FeatherOS's own",
        "FFNT format. tools/fonts/generate.py pre-rendered them from the",
        "TrueType fonts below, which are licensed under the SIL Open Font",
        "License 1.1. They are not the original fonts and are named mono-N",
        "and sans-N after their family kind and line height in pixels.",
        "",
    ]
    for family in FAMILIES:
        copyright_line = (
            (SOURCES / family.licence)
            .read_text(encoding="utf-8")
            .splitlines()[0]
            .strip()
        )
        version = versions[family.ttf][1]
        lines += [
            f"{family.name}-*.ffnt: derived from {family.display} Regular "
            f"{version}",
            f"  {copyright_line}",
            f"  Licence: {family.licence_output}",
            "",
        ]
    lines += [
        "sample.txt is plain text for the FONTS smoke check, not a font:",
        "characters the console font has, and some invalid UTF-8 bytes.",
        "",
        "The font files are under the SIL Open Font License 1.1, not under",
        "the licence of the rest of FeatherOS.",
        "SHA256SUMS lists the SHA-256 of every other file in this folder.",
    ]
    return "\n".join(lines) + "\n"


def c_source(data: bytes, notice: str) -> str:
    lines = []
    for start in range(0, len(data), 16):
        chunk = data[start : start + 16]
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    body = "\n".join(lines)
    wrapped_notice = "\n".join(
        textwrap.wrap(
            notice,
            96,
            initial_indent="// ",
            subsequent_indent="// ",
            break_on_hyphens=False,
        )
    )
    return (
        "// Generated by tools/fonts/generate.py from "
        "initramfs/fonts/mono-16.ffnt; do not edit.\n"
        f"{wrapped_notice}\n"
        "\n"
        '#include "kernel/framebuffer/console_font.h"\n'
        "\n"
        "const uint8_t ConsoleFontData[] = {\n"
        f"{body}\n"
        "};\n"
        "\n"
        "const size_t ConsoleFontSize = sizeof(ConsoleFontData);\n"
    )


def c_header() -> str:
    width, height = CONSOLE_CELL
    return f"""\
// Generated by tools/fonts/generate.py; do not edit.
#ifndef FEATHEROS_KERNEL_FRAMEBUFFER_CONSOLE_FONT_H
#define FEATHEROS_KERNEL_FRAMEBUFFER_CONSOLE_FONT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{{
#endif

    enum
    {{
        ConsoleFontCellWidth = {width},
        ConsoleFontCellHeight = {height},
    }};

    extern const uint8_t ConsoleFontData[];
    extern const size_t ConsoleFontSize;

#ifdef __cplusplus
}}
#endif

#endif
"""


def reference_text(fonts: dict[str, ffnt.Font]) -> str:
    lines = [
        "# FFNT reference renderings, written by tools/fonts/generate.py.",
        "# Each line: font, FNV-1a 32 of the pixels, width, height, text,",
        "# tab-separated. The text is drawn at the margin 4 px from the",
        "# top and left of a surface of the measured width plus 8 and the",
        "# line height plus 8, filled with "
        f"0x{REFERENCE_BACKGROUND:06X}, in 0x{REFERENCE_FOREGROUND:06X};",
        "# C0 controls and DEL are skipped, a code point the font lacks",
        "# draws record 0. The checksum is Surface::Checksum's.",
    ]
    for name in sorted(fonts):
        for text in REFERENCE_TEXTS:
            width, height, pixels = ffnt.render_text(
                fonts[name], text, REFERENCE_BACKGROUND, REFERENCE_FOREGROUND
            )
            lines.append(
                f"{name}\t{ffnt.checksum32(pixels):08x}\t{width}\t{height}"
                f"\t{text}"
            )
    return "\n".join(lines) + "\n"


def generate(out: Path, console_c: Path, console_h: Path, reference: Path):
    versions = read_sources()
    fonts_dir = out
    fonts_dir.mkdir(parents=True, exist_ok=True)
    files: dict[str, bytes] = {}
    parsed: dict[str, ffnt.Font] = {}
    report = []
    for family in FAMILIES:
        source = Source(SOURCES / family.ttf)
        notice = notice_for(family, versions)
        for size in family.sizes:
            built = build(family, source, size, notice)
            data = ffnt.write(built.font)
            name = f"{family.name}-{size}"
            files[f"{name}.ffnt"] = data
            parsed[name] = ffnt.read(data)
            report.append((name, built, len(data)))
        files[family.licence_output] = (SOURCES / family.licence).read_bytes()
    files["sample.txt"] = (HERE / "sample.txt").read_bytes()
    files["README.txt"] = readme_text(versions).encode("utf-8")
    sums = "".join(
        f"{hashlib.sha256(files[name]).hexdigest()}  {name}\n"
        for name in sorted(files)
    )
    files["SHA256SUMS"] = sums.encode("utf-8")
    for name, data in files.items():
        (fonts_dir / name).write_bytes(data)
    console = files[f"{CONSOLE_FAMILY}-{CONSOLE_SIZE}.ffnt"]
    notice = parsed[f"{CONSOLE_FAMILY}-{CONSOLE_SIZE}"].notice
    console_c.write_text(c_source(console, notice), encoding="utf-8")
    console_h.write_text(c_header(), encoding="utf-8")
    reference.parent.mkdir(parents=True, exist_ok=True)
    reference.write_text(reference_text(parsed), encoding="utf-8")
    return report


def print_report(report) -> None:
    total = 0
    for name, built, size in report:
        font = built.font
        total += size
        assigned = [c for c in built.absent if unicodedata.name(chr(c), "")]
        unassigned = len(built.absent) - len(assigned)
        print(
            f"{name}: {len(font.glyphs)} glyphs, {size} bytes, em "
            f"{built.em:.4f}, ascent {font.ascent}, descent {font.descent}"
        )
        if assigned:
            listing = " ".join(f"U+{c:04X}" for c in assigned)
            print(f"  lacks {len(assigned)} assigned: {listing}")
        if unassigned:
            print(f"  {unassigned} requested code points are unassigned")
        if built.clipped:
            listing = " ".join(f"U+{c:04X}" for c in built.clipped)
            print(f"  clipped {len(built.clipped)}: {listing}")
    print(f"total {total} bytes in {len(report)} files")


def compare(expected: Path, actual: Path, label: str) -> list[str]:
    if not actual.exists():
        return [f"{label} is missing"]
    if expected.read_bytes() != actual.read_bytes():
        return [f"{label} differs"]
    return []


def use_virtual_environment() -> None:
    try:
        import freetype  # noqa: F401
    except ImportError:
        python = HERE / ".venv" / "bin" / "python"
        if python.exists() and "FEATHEROS_FONTS_VENV" not in os.environ:
            os.environ["FEATHEROS_FONTS_VENV"] = "1"
            os.execv(str(python), [str(python), *sys.argv])


def check() -> int:
    use_virtual_environment()
    try:
        import freetype  # noqa: F401
    except ImportError:
        print("generate.py: freetype-py is not installed, skipping")
        return SKIP
    with tempfile.TemporaryDirectory() as folder:
        temp = Path(folder)
        generate(
            temp / "fonts",
            temp / "console_font.c",
            temp / "console_font.h",
            temp / "font_reference.txt",
        )
        problems = []
        names = sorted(p.name for p in (temp / "fonts").iterdir())
        existing = sorted(p.name for p in FONTS.iterdir())
        if names != existing:
            problems.append(f"files {existing} differ from {names}")
        for name in names:
            problems += compare(temp / "fonts" / name, FONTS / name, name)
        problems += compare(
            temp / "console_font.c", CONSOLE_C, "console_font.c"
        )
        problems += compare(
            temp / "console_font.h", CONSOLE_H, "console_font.h"
        )
        problems += compare(
            temp / "font_reference.txt", REFERENCE, "font_reference.txt"
        )
    for problem in problems:
        print(f"generate.py: {problem}")
    return 1 if problems else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if args.check:
        return check()
    use_virtual_environment()
    try:
        import freetype  # noqa: F401
    except ImportError:
        die(
            "freetype-py is missing: python3 -m venv tools/fonts/.venv && "
            "tools/fonts/.venv/bin/pip install -r "
            "tools/fonts/requirements.txt"
        )
    print_report(generate(FONTS, CONSOLE_C, CONSOLE_H, REFERENCE))
    return 0


if __name__ == "__main__":
    sys.exit(main())
