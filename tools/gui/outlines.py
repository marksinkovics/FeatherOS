#!/usr/bin/env python3
"""Writes the outlines FeatherOS draws its artwork from (docs/design/M37.md
1.7): initramfs/gui/outlines.fvo, initramfs/gui/SHA256SUMS and the generated
src/user/ui/glyph_ids.h, from the sources in tools/gui/art/.

outlines.py          write the three files
outlines.py --check  compare them with what the sources produce

Every host must write the same bytes (invariant I12), so the script uses only
+ - * / and sqrt on doubles, rounds to the 1/256 integer grid with floor, and
iterates in sorted order. Curves are flattened to within 1/4096 of the
drawing's width; elliptical arcs are bisected with half-angle vectors, never
with sin, cos or atan2.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import xml.etree.ElementTree as ElementTree
from math import ceil, floor, sqrt
from pathlib import Path

import fvol

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
ART = HERE / "art"
OUT_DIR = ROOT / "initramfs" / "gui"
OUTLINES_FILE = OUT_DIR / "outlines.fvo"
SUMS_FILE = OUT_DIR / "SHA256SUMS"
GLYPH_HEADER = ROOT / "src" / "user" / "ui" / "glyph_ids.h"
POINTER_HEADER = ROOT / "src" / "user" / "ui" / "pointer_ids.h"
CURSORS = ROOT / "docs" / "design" / "gui" / "cursors"

# A curve deviates from its polyline by at most the view box / this.
FLATNESS_DIVISOR = 4096
MAXIMUM_CUBIC_SEGMENTS = 1024
MAXIMUM_ARC_DEPTH = 14
# Below this the two arc end vectors are taken to be opposite.
OPPOSITE = 1e-6

GLYPH_PREFIX = "glyph."
FEATHER_NAME = "feather"
POINTER_PREFIX = "pointer."

SVG_NAMESPACE = "{http://www.w3.org/2000/svg}"
# The pointers' accent parts (docs/design/gui/cursors/cursors.json) are this
# colour in the files; the drawing paints them from slot 0.
ACCENT_SOURCE = 0xF0A840
SPINNER_FRAMES = 8
# Pointer name in cursors.json -> PointerId enumerator; the spinner pointers
# carry their frames as drawings "pointer.<name>.<frame>".
POINTER_IDS = {
    "default": "Arrow",
    "pointer": "Link",
    "text": "Text",
    "progress": "Working",
    "wait": "Busy",
    "grab": "Move",
    "grabbing": "Moving",
    "ns": "ResizeVertical",
    "ew": "ResizeHorizontal",
    "nwse": "ResizeNwse",
    "nesw": "ResizeNesw",
    "no": "NotAllowed",
    "cross": "Precise",
}
SPINNERS = ("progress", "wait")

NUMBER = re.compile(r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?")
SEPARATORS = " \t\r\n,"
ARGUMENT_COUNTS = {
    "m": 2,
    "l": 2,
    "h": 1,
    "v": 1,
    "c": 6,
    "s": 4,
    "a": 7,
    "z": 0,
}


class SourceError(Exception):
    pass


def snap(value: float) -> int:
    return floor(value * fvol.UNIT + 0.5)


class Scanner:
    def __init__(self, text: str) -> None:
        self.text = text
        self.position = 0

    def skip(self) -> None:
        while (
            self.position < len(self.text)
            and self.text[self.position] in SEPARATORS
        ):
            self.position += 1

    def command(self) -> str | None:
        self.skip()
        if self.position >= len(self.text):
            return None
        char = self.text[self.position]
        if not char.isalpha():
            return ""
        self.position += 1
        return char

    def at_number(self) -> bool:
        self.skip()
        return (
            self.position < len(self.text)
            and NUMBER.match(self.text, self.position) is not None
        )

    def number(self) -> float:
        self.skip()
        match = NUMBER.match(self.text, self.position)
        if match is None:
            raise SourceError(f"number expected at {self.position}")
        self.position = match.end()
        return float(match.group())

    def flag(self) -> int:
        self.skip()
        char = self.text[self.position : self.position + 1]
        if char not in ("0", "1"):
            raise SourceError(f"arc flag expected at {self.position}")
        self.position += 1
        return int(char)


def flatten_cubic(p0, p1, p2, p3, tolerance):
    """The points after p0, ending at p3, of n equal steps in the parameter."""
    first = sqrt(
        (p0[0] - 2 * p1[0] + p2[0]) * (p0[0] - 2 * p1[0] + p2[0])
        + (p0[1] - 2 * p1[1] + p2[1]) * (p0[1] - 2 * p1[1] + p2[1])
    )
    second = sqrt(
        (p1[0] - 2 * p2[0] + p3[0]) * (p1[0] - 2 * p2[0] + p3[0])
        + (p1[1] - 2 * p2[1] + p3[1]) * (p1[1] - 2 * p2[1] + p3[1])
    )
    bend = first if first > second else second
    steps = ceil(sqrt(0.75 * bend / tolerance))
    steps = min(max(steps, 1), MAXIMUM_CUBIC_SEGMENTS)
    points = []
    for index in range(1, steps):
        t = index / steps
        u = 1 - t
        a = u * u * u
        b = 3 * u * u * t
        c = 3 * u * t * t
        d = t * t * t
        points.append(
            (
                a * p0[0] + b * p1[0] + c * p2[0] + d * p3[0],
                a * p0[1] + b * p1[1] + c * p2[1] + d * p3[1],
            )
        )
    points.append(p3)
    return points


def unit(x: float, y: float) -> tuple[float, float]:
    length = sqrt(x * x + y * y)
    return x / length, y / length


def flatten_arc(start, rx, ry, rotation, large, sweep, end, tolerance):
    """The points after `start`, ending at `end`, of an elliptical arc (the
    SVG endpoint parameterization with a horizontal x axis)."""
    if rotation != 0:
        raise SourceError("an arc with a rotated axis is not supported")
    if start == end:
        return []
    rx = abs(rx)
    ry = abs(ry)
    if rx == 0 or ry == 0:
        return [end]
    half_x = (start[0] - end[0]) / 2
    half_y = (start[1] - end[1]) / 2
    excess = half_x * half_x / (rx * rx) + half_y * half_y / (ry * ry)
    half_circle = excess > 1
    if half_circle:
        factor = sqrt(excess)
        rx = rx * factor
        ry = ry * factor
    rx2 = rx * rx
    ry2 = ry * ry
    denominator = rx2 * half_y * half_y + ry2 * half_x * half_x
    ratio = (rx2 * ry2 - denominator) / denominator
    coefficient = sqrt(ratio) if ratio > 0 and not half_circle else 0.0
    if large == sweep:
        coefficient = -coefficient
    prime_x = coefficient * rx * half_y / ry
    prime_y = -coefficient * ry * half_x / rx
    u1 = unit((half_x - prime_x) / rx, (half_y - prime_y) / ry)
    u2 = unit((-half_x - prime_x) / rx, (-half_y - prime_y) / ry)
    direction = 1.0 if sweep else -1.0
    centre_x = prime_x + (start[0] + end[0]) / 2
    centre_y = prime_y + (start[1] + end[1]) / 2
    radius = rx if rx > ry else ry

    def at(vector):
        return (centre_x + rx * vector[0], centre_y + ry * vector[1])

    out = []

    def emit(a, b, depth):
        cosine_squared = (1 + a[0] * b[0] + a[1] * b[1]) / 2
        cosine = sqrt(cosine_squared if cosine_squared > 0 else 0.0)
        if depth < MAXIMUM_ARC_DEPTH and radius * (1 - cosine) > tolerance:
            bisector = unit(a[0] + b[0], a[1] + b[1])
            emit(a, bisector, depth + 1)
            emit(bisector, b, depth + 1)
        else:
            out.append(at(b))

    sum_x = u1[0] + u2[0]
    sum_y = u1[1] + u2[1]
    opposite = sqrt(sum_x * sum_x + sum_y * sum_y) < OPPOSITE
    if half_circle:
        u2 = (-u1[0], -u1[1])
        opposite = True
    if opposite:
        mid = (-direction * u1[1], direction * u1[0])
    elif large:
        mid = unit(-sum_x, -sum_y)
    else:
        mid = None
    if mid is None:
        emit(u1, u2, 0)
    else:
        emit(u1, mid, 0)
        emit(mid, u2, 0)
    out[-1] = end
    return out


def parse_path(text: str, tolerance: float):
    """Subpaths of a path as (points, closed), flattened, in floats."""
    scanner = Scanner(text)
    subpaths: list[tuple[list, bool]] = []
    points: list | None = None
    current = (0.0, 0.0)
    start = current
    control = None
    while True:
        letter = scanner.command()
        if letter is None:
            break
        if letter == "":
            raise SourceError(f"unexpected character at {scanner.position}")
        lower = letter.lower()
        if lower not in ARGUMENT_COUNTS:
            raise SourceError(f"unsupported command {letter!r}")
        relative = letter != letter.upper()
        if lower == "z":
            if points is not None:
                subpaths.append((points, True))
            points = None
            current = start
            control = None
            continue
        first = True
        while first or scanner.at_number():
            first = False
            if lower == "m":
                x, y = scanner.number(), scanner.number()
                if relative:
                    x, y = current[0] + x, current[1] + y
                if points is not None:
                    subpaths.append((points, False))
                current = start = (x, y)
                points = [current]
                control = None
                lower = "l"
                continue
            if points is None:
                points = [current]
                start = current
            if lower == "l":
                x, y = scanner.number(), scanner.number()
                target = (
                    (current[0] + x, current[1] + y) if relative else (x, y)
                )
                points.append(target)
                control = None
            elif lower == "h":
                x = scanner.number()
                target = (current[0] + x if relative else x, current[1])
                points.append(target)
                control = None
            elif lower == "v":
                y = scanner.number()
                target = (current[0], current[1] + y if relative else y)
                points.append(target)
                control = None
            elif lower in "cs":
                if lower == "c":
                    x1, y1 = scanner.number(), scanner.number()
                    if relative:
                        x1, y1 = current[0] + x1, current[1] + y1
                    first_control = (x1, y1)
                else:
                    first_control = (
                        (
                            2 * current[0] - control[0],
                            2 * current[1] - control[1],
                        )
                        if control is not None
                        else current
                    )
                x2, y2 = scanner.number(), scanner.number()
                x, y = scanner.number(), scanner.number()
                if relative:
                    x2, y2 = current[0] + x2, current[1] + y2
                    x, y = current[0] + x, current[1] + y
                target = (x, y)
                points += flatten_cubic(
                    current, first_control, (x2, y2), target, tolerance
                )
                control = (x2, y2)
            else:
                rx, ry = scanner.number(), scanner.number()
                rotation = scanner.number()
                large, sweep = scanner.flag(), scanner.flag()
                x, y = scanner.number(), scanner.number()
                target = (
                    (current[0] + x, current[1] + y) if relative else (x, y)
                )
                points += flatten_arc(
                    current, rx, ry, rotation, large, sweep, target, tolerance
                )
                control = None
            current = target
            if lower not in "cs":
                control = None
    if points is not None:
        subpaths.append((points, False))
    return subpaths


def to_polylines(subpaths, stroke: bool) -> list[fvol.Polyline]:
    polylines = []
    for points, closed in subpaths:
        snapped: list[tuple[int, int]] = []
        for x, y in points:
            point = (snap(x), snap(y))
            if not snapped or snapped[-1] != point:
                snapped.append(point)
        if closed and len(snapped) > 1 and snapped[0] == snapped[-1]:
            snapped.pop()
        if len(snapped) == 1 and stroke and not closed:
            snapped.append(snapped[0])
        if len(snapped) < (3 if closed else 2):
            continue
        polylines.append(fvol.Polyline(snapped, closed))
    return polylines


def load_json(name: str) -> dict:
    return json.loads((ART / name).read_text(encoding="utf-8"))


def glyph_drawings(icons: dict) -> list[fvol.Drawing]:
    grid = icons["grid"]
    tolerance = grid / FLATNESS_DIVISOR
    width = snap(icons["stroke_width"])
    drawings = []
    for glyph in icons["glyphs"]:
        polylines = to_polylines(
            parse_path(glyph["path"], tolerance), stroke=True
        )
        if not polylines:
            raise SourceError(f"glyph {glyph['name']} has no outline")
        item = fvol.Item(True, fvol.CURRENT, width=width, polylines=polylines)
        drawings.append(
            fvol.Drawing(GLYPH_PREFIX + glyph["name"], grid, grid, [item])
        )
    return drawings


def paint_fields(paint: str) -> tuple[int, int]:
    if paint == "current":
        return fvol.CURRENT, 0
    match = re.fullmatch(r"slot([0-7])", paint)
    if match:
        return fvol.SLOT, int(match.group(1))
    raise SourceError(f"unknown paint {paint!r}")


def feather_drawing(feather: dict) -> fvol.Drawing:
    box = feather["view_box"]
    tolerance = box / FLATNESS_DIVISOR
    items = []
    for entry in feather["items"]:
        stroke = entry["kind"] == "stroke"
        paint, slot = paint_fields(entry["paint"])
        items.append(
            fvol.Item(
                stroke,
                paint,
                slot,
                width=snap(entry["width"]) if stroke else 0,
                polylines=to_polylines(
                    parse_path(entry["path"], tolerance), stroke
                ),
            )
        )
    return fvol.Drawing(feather["name"], box, box, items)


def wallpaper_drawing(wallpaper: dict) -> fvol.Drawing:
    width, height = wallpaper["width"], wallpaper["height"]
    tolerance = max(width, height) / FLATNESS_DIVISOR
    items = []
    for entry in wallpaper["items"]:
        stroke = entry["kind"] == "stroke"
        paint, slot = paint_fields(entry["paint"])
        items.append(
            fvol.Item(
                stroke,
                paint,
                slot,
                width=snap(entry["width"]) if stroke else 0,
                polylines=to_polylines(
                    parse_path(entry["path"], tolerance), stroke
                ),
            )
        )
    return fvol.Drawing(wallpaper["name"], width, height, items)


def rotation_of(degrees: float) -> tuple[float, float]:
    """(cos, sin) of a multiple of 45 degrees, from sqrt(1/2) alone."""
    steps = degrees / 45
    if steps != int(steps):
        raise SourceError(f"rotate({degrees:g}) is not a multiple of 45")
    root = sqrt(0.5)
    return [
        (1.0, 0.0),
        (root, root),
        (0.0, 1.0),
        (-root, root),
        (-1.0, 0.0),
        (-root, -root),
        (0.0, -1.0),
        (root, -root),
    ][int(steps) % 8]


ROTATE = re.compile(
    r"rotate\(\s*(%s)(?:[\s,]+(%s)[\s,]+(%s))?\s*\)\s*"
    % ((NUMBER.pattern,) * 3)
)


def parse_rotate(text: str) -> tuple[float, float, float]:
    match = ROTATE.fullmatch(text.strip())
    if match is None:
        raise SourceError(f"unsupported transform {text!r}")
    centre_x, centre_y = match.group(2), match.group(3)
    return (
        float(match.group(1)),
        float(centre_x) if centre_x else 0.0,
        float(centre_y) if centre_y else 0.0,
    )


def rotate_subpaths(subpaths, degrees, centre_x, centre_y):
    cosine, sine = rotation_of(degrees)
    return [
        (
            [
                (
                    centre_x + (x - centre_x) * cosine - (y - centre_y) * sine,
                    centre_y + (x - centre_x) * sine + (y - centre_y) * cosine,
                )
                for x, y in points
            ],
            closed,
        )
        for points, closed in subpaths
    ]


def circle_subpaths(element, tolerance):
    cx = float(element.get("cx", "0"))
    cy = float(element.get("cy", "0"))
    r = float(element.get("r", "0"))
    if r <= 0:
        raise SourceError("a circle needs a positive r")
    left = (cx - r, cy)
    right = (cx + r, cy)
    points = [left]
    points += flatten_arc(left, r, r, 0.0, 1, 1, right, tolerance)
    points += flatten_arc(right, r, r, 0.0, 1, 1, left, tolerance)
    return [(points, True)]


STYLE_DEFAULTS = {
    "fill": "#000",
    "stroke": "none",
    "stroke-width": "1",
    "stroke-linecap": "butt",
    "stroke-linejoin": "miter",
    "fill-opacity": "1",
    "stroke-opacity": "1",
}
GROUP_ATTRIBUTES = set(STYLE_DEFAULTS)
LEAF_ATTRIBUTES = GROUP_ATTRIBUTES | {
    "d",
    "cx",
    "cy",
    "r",
    "opacity",
    "paint-order",
    "transform",
}


def svg_paint(value: str) -> tuple[int, int, int]:
    """(paint kind, slot, ARGB) of a #rgb or #rrggbb colour."""
    match = re.fullmatch(r"#([0-9a-fA-F]{3}|[0-9a-fA-F]{6})", value)
    if match is None:
        raise SourceError(f"unsupported colour {value!r}")
    digits = match.group(1)
    if len(digits) == 3:
        digits = "".join(c + c for c in digits)
    rgb = int(digits, 16)
    if rgb == ACCENT_SOURCE:
        return fvol.SLOT, 0, 0
    return fvol.FIXED, 0, 0xFF000000 | rgb


def svg_opacity(value: float) -> int:
    return floor(value * 255 + 0.5)


def svg_items(element, style, tolerance, spinner_angle):
    """The items of one shape in paint order; `spinner_angle` replaces the
    angle of a rotate transform when not None."""
    tag = element.tag.removeprefix(SVG_NAMESPACE)
    unknown = set(element.attrib) - LEAF_ATTRIBUTES
    if unknown:
        raise SourceError(f"<{tag}> has unsupported {sorted(unknown)}")
    own = dict(style)
    own.update({k: v for k, v in element.attrib.items() if k in own})
    if tag == "path":
        subpaths = parse_path(element.get("d", ""), tolerance)
    elif tag == "circle":
        subpaths = circle_subpaths(element, tolerance)
    else:
        raise SourceError(f"unsupported element <{tag}>")
    if "transform" in element.attrib:
        degrees, centre_x, centre_y = parse_rotate(element.get("transform"))
        if spinner_angle is not None:
            degrees = spinner_angle
        subpaths = rotate_subpaths(subpaths, degrees, centre_x, centre_y)
    opacity = float(element.get("opacity", "1"))
    filled = own["fill"] != "none"
    stroked = own["stroke"] != "none"
    if opacity != 1 and filled and stroked:
        raise SourceError("opacity on a filled and stroked shape")
    items = []
    if filled:
        paint, slot, colour = svg_paint(own["fill"])
        closed = [(points, True) for points, _ in subpaths]
        items.append(
            fvol.Item(
                False,
                paint,
                slot,
                colour,
                svg_opacity(opacity * float(own["fill-opacity"])),
                0,
                to_polylines(closed, stroke=False),
            )
        )
    if stroked:
        paint, slot, colour = svg_paint(own["stroke"])
        round_caps = own["stroke-linecap"] == "round"
        # A circle and an arc have no corner for a join to shape.
        round_joins = (
            own["stroke-linejoin"] == "round"
            or tag == "circle"
            or re.search("[aA]", element.get("d", "")) is not None
        )
        for points, closed in subpaths:
            if not closed and not round_caps:
                raise SourceError("an open stroke needs round caps")
            if (closed or len(points) > 2) and not round_joins:
                raise SourceError("a stroked corner needs round joins")
        items.append(
            fvol.Item(
                True,
                paint,
                slot,
                colour,
                svg_opacity(opacity * float(own["stroke-opacity"])),
                snap(float(own["stroke-width"])),
                to_polylines(subpaths, stroke=True),
            )
        )
        if filled and element.get("paint-order") == "stroke":
            items.reverse()
    return items


def svg_drawing(name, text, hotspot, spinner_angle=None):
    root = ElementTree.fromstring(text)
    if root.tag != SVG_NAMESPACE + "svg":
        raise SourceError(f"{name}: not an svg")
    box = root.get("viewBox", "").split()
    if len(box) != 4 or box[0] != "0" or box[1] != "0" or box[2] != box[3]:
        raise SourceError(f"{name}: the viewBox must be 0 0 N N")
    side = int(box[2])
    tolerance = side / FLATNESS_DIVISOR
    items = []

    def walk(element, style):
        for child in element:
            tag = child.tag.removeprefix(SVG_NAMESPACE)
            if tag == "g":
                bad = set(child.attrib) - GROUP_ATTRIBUTES
                if bad:
                    raise SourceError(f"<g> has unsupported {sorted(bad)}")
                inner = dict(style)
                inner.update(
                    {k: v for k, v in child.attrib.items() if k in inner}
                )
                walk(child, inner)
            else:
                items.extend(svg_items(child, style, tolerance, spinner_angle))

    walk(root, dict(STYLE_DEFAULTS))
    for item in items:
        if not item.polylines:
            raise SourceError(f"{name}: an item has no outline")
    return fvol.Drawing(
        name,
        side,
        side,
        items,
        (hotspot[0] * fvol.UNIT, hotspot[1] * fvol.UNIT),
    )


def pointer_sources() -> list[dict]:
    cursors = json.loads((CURSORS / "cursors.json").read_text("utf-8"))
    entries = cursors["cursors"]
    if [c["name"] for c in entries] != list(POINTER_IDS):
        raise SourceError(
            "cursors.json and POINTER_IDS list different pointers"
        )
    return entries


def pointer_drawing_names(name: str) -> list[str]:
    if name in SPINNERS:
        return [f"{POINTER_PREFIX}{name}.{k}" for k in range(SPINNER_FRAMES)]
    return [POINTER_PREFIX + name]


def pointer_drawings() -> list[fvol.Drawing]:
    drawings = []
    for cursor in pointer_sources():
        name = cursor["name"]
        text = (CURSORS / "svg" / f"{name}.svg").read_text("utf-8")
        hotspot = tuple(cursor["hotspot_at_24px"])
        for frame, drawing_name in enumerate(pointer_drawing_names(name)):
            angle = 45 * frame if name in SPINNERS else None
            drawings.append(svg_drawing(drawing_name, text, hotspot, angle))
    return drawings


def build_drawings() -> tuple[list[fvol.Drawing], dict]:
    icons = load_json("icons.json")
    drawings = glyph_drawings(icons)
    drawings.append(feather_drawing(load_json("feather.json")))
    drawings.append(wallpaper_drawing(load_json("wallpaper.json")))
    drawings += pointer_drawings()
    names = [d.name for d in drawings]
    if len(set(names)) != len(names):
        raise SourceError("two drawings share a name")
    return drawings, icons


def pascal(name: str) -> str:
    return name[:1].upper() + name[1:]


def render_glyph_header(icons: dict) -> str:
    names = [glyph["name"] for glyph in icons["glyphs"]]
    ids = [pascal(name) for name in names]
    if len(set(ids)) != len(ids):
        raise SourceError("two glyphs share an identifier")
    out = [
        "// Generated by tools/gui/outlines.py from tools/gui/art/icons.json. "
        "Do not edit.",
        "#ifndef FEATHEROS_USER_UI_GLYPH_IDS_H",
        "#define FEATHEROS_USER_UI_GLYPH_IDS_H",
        "",
        "#include <stdint.h>",
        "",
        "namespace FeatherOS::UI",
        "{",
        "",
        "enum class GlyphId : uint8_t",
        "{",
    ]
    out += [f"    {glyph_id}," for glyph_id in ids]
    out += [
        "};",
        "",
        f"constexpr uint32_t GlyphCount = {len(ids)};",
        "",
        "// The name of each glyph's drawing in the file, by GlyphId.",
        "// clang-format off",
        "inline constexpr const char* GlyphDrawingNames[GlyphCount] = {",
    ]
    out += [f'    "{GLYPH_PREFIX}{name}",' for name in names]
    out += [
        "};",
        "// clang-format on",
        "",
        "} // namespace FeatherOS::UI",
        "",
        "#endif",
        "",
    ]
    return "\n".join(out)


def render_pointer_header() -> str:
    entries = pointer_sources()
    ids = [POINTER_IDS[c["name"]] for c in entries]
    names = []
    first = []
    frames = []
    for cursor in entries:
        first.append(len(names))
        group = pointer_drawing_names(cursor["name"])
        frames.append(len(group))
        names += group
    out = [
        "// Generated by tools/gui/outlines.py from docs/design/gui/cursors/. "
        "Do not edit.",
        "#ifndef FEATHEROS_USER_UI_POINTER_IDS_H",
        "#define FEATHEROS_USER_UI_POINTER_IDS_H",
        "",
        "#include <stdint.h>",
        "",
        "namespace FeatherOS::UI",
        "{",
        "",
        "enum class PointerId : uint8_t",
        "{",
    ]
    out += [f"    {pointer_id}," for pointer_id in ids]
    out += [
        "};",
        "",
        f"constexpr uint32_t PointerCount = {len(ids)};",
        f"constexpr uint32_t PointerDrawingCount = {len(names)};",
        "",
        "// A pointer's drawings are consecutive in PointerDrawingNames, from",
        "// PointerFirstDrawing; a spinner has one per frame.",
        "// clang-format off",
        "inline constexpr uint8_t PointerFirstDrawing[PointerCount] = {",
        "    " + ", ".join(str(n) for n in first) + ",",
        "};",
        "inline constexpr uint8_t PointerFrames[PointerCount] = {",
        "    " + ", ".join(str(n) for n in frames) + ",",
        "};",
        "inline constexpr const char*"
        " PointerDrawingNames[PointerDrawingCount] = {",
    ]
    out += [f'    "{name}",' for name in names]
    out += [
        "};",
        "// clang-format on",
        "",
        "} // namespace FeatherOS::UI",
        "",
        "#endif",
        "",
    ]
    return "\n".join(out)


def outputs() -> dict[Path, bytes]:
    drawings, icons = build_drawings()
    blob = fvol.write(drawings)
    sums = f"{hashlib.sha256(blob).hexdigest()}  {OUTLINES_FILE.name}\n"
    return {
        OUTLINES_FILE: blob,
        SUMS_FILE: sums.encode("utf-8"),
        GLYPH_HEADER: render_glyph_header(icons).encode("utf-8"),
        POINTER_HEADER: render_pointer_header().encode("utf-8"),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument(
        "--check", action="store_true", help="compare, change nothing"
    )
    args = parser.parse_args()
    try:
        files = outputs()
    except (SourceError, fvol.FvolError, KeyError, ValueError) as error:
        print(f"FAIL {error}")
        return 1
    stale = []
    for path, data in files.items():
        current = path.read_bytes() if path.exists() else None
        if current != data:
            stale.append(path)
            if not args.check:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
    names = ", ".join(str(path.relative_to(ROOT)) for path in stale)
    if args.check:
        if stale:
            print(f"FAIL not what outlines.py generates: {names}")
            return 1
        print("outlines ok: the files are what the sources generate")
        return 0
    print(f"wrote {names}" if stale else "outlines unchanged")
    return 0


if __name__ == "__main__":
    sys.exit(main())
