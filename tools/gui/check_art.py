#!/usr/bin/env python3
"""Checks the artwork (CTests gui.files and gui.prototype).

check_art.py files      initramfs/gui: SHA256SUMS, the outlines file parses
                        with fvol.py, and every drawing the sources (icons,
                        feather, pointers with their hotspots) and the C++
                        code ask for exists
check_art.py prototype  tools/gui/art/icons.json, feather.json and
                        wallpaper.json equal the prototype's P table, VANE,
                        featherSvg and wallpaper

The script reports a failure and never changes a file."""

from __future__ import annotations

import hashlib
import json
import re
import sys
from pathlib import Path

import fvol
import outlines

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
ART = HERE / "art"
GUI = ROOT / "initramfs" / "gui"
UI_SOURCES = ROOT / "src" / "user" / "ui"
PROTOTYPE = ROOT / "docs" / "design" / "gui" / "prototype.html"

GLYPH_PREFIX = "glyph."
FEATHER_NAME = "feather"
WALLPAPER_NAME = "wallpaper"


def check_sums() -> list[str]:
    sums = GUI / "SHA256SUMS"
    if not sums.exists():
        return ["initramfs/gui/SHA256SUMS is missing"]
    listed = {}
    for line in sums.read_text(encoding="utf-8").splitlines():
        digest, _, name = line.partition("  ")
        listed[name] = digest
    problems = []
    for name, digest in sorted(listed.items()):
        path = GUI / name
        if not path.exists():
            problems.append(f"SHA256SUMS lists {name}, which is missing")
        elif hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            problems.append(f"{name} does not match SHA256SUMS")
    for path in sorted(GUI.iterdir()):
        if path.name != "SHA256SUMS" and path.name not in listed:
            problems.append(f"{path.name} is not in SHA256SUMS")
    return problems


def wanted_names() -> dict[str, str]:
    """Drawing name -> who asks for it."""
    icons = json.loads((ART / "icons.json").read_text(encoding="utf-8"))
    wanted = {
        GLYPH_PREFIX + glyph["name"]: "icons.json" for glyph in icons["glyphs"]
    }
    wanted[FEATHER_NAME] = "feather.json"
    wanted[WALLPAPER_NAME] = "wallpaper.json"
    for cursor in outlines.pointer_sources():
        for name in outlines.pointer_drawing_names(cursor["name"]):
            wanted[name] = "cursors.json"
    for path in sorted(UI_SOURCES.glob("*.cpp")):
        for name in re.findall(
            r'\.Find\("([^"]+)"\)', path.read_text("utf-8")
        ):
            wanted.setdefault(name, path.name)
    return wanted


def check_files() -> list[str]:
    problems = check_sums()
    path = GUI / "outlines.fvo"
    if not path.exists():
        return problems + ["initramfs/gui/outlines.fvo is missing"]
    try:
        drawings = fvol.read(path.read_bytes())
    except fvol.FvolError as error:
        return problems + [f"outlines.fvo is refused: {error}"]
    have = {drawing.name for drawing in drawings}
    for drawing in drawings:
        if drawing.name.startswith(outlines.POINTER_PREFIX) and (
            drawing.hotspot is None
        ):
            problems.append(f"{drawing.name} has no hotspot")
    for name, who in sorted(wanted_names().items()):
        if name not in have:
            problems.append(f"{who} asks for {name}, which the file lacks")
    return problems


def parse_prototype(text: str) -> dict:
    table = re.search(r"const P = \{(.*?)\n\};", text, re.S).group(1)
    vane = re.search(r"const VANE = '([^']*)'", text).group(1)
    feather = re.search(
        r"function featherSvg\(s\) \{ return `(.*?)`; \}", text
    ).group(1)
    strokes = re.findall(
        r'<path fill="none" stroke="[^"]*" stroke-width="([\d.]+)"'
        r' stroke-linecap="round" d="([^"]*)"/>',
        feather,
    )
    return {
        "glyphs": re.findall(r"(\w+):'([^']*)'", table),
        "grid": re.search(
            r'class="i" width="\$\{s\}" height="\$\{s\}" '
            r'viewBox="0 0 (\d+) (\d+)"',
            text,
        ).groups(),
        "stroke_width": re.search(
            r"\n\.i\{[^}]*stroke-width:([\d.]+)", text
        ).group(1),
        "vane": vane,
        "feather_box": re.search(
            r'viewBox="0 0 (\d+) (\d+)"', feather
        ).groups(),
        "feather_fill": 'fill="currentColor" d="${VANE}"' in feather,
        "feather_strokes": strokes,
        "wallpaper": re.search(
            r'<svg id="wall" viewBox="0 0 (\d+) (\d+)"[^>]*>(.*?)</svg>',
            text,
            re.S,
        ).groups(),
    }


def check_prototype() -> list[str]:
    proto = parse_prototype(PROTOTYPE.read_text(encoding="utf-8"))
    icons = json.loads((ART / "icons.json").read_text(encoding="utf-8"))
    feather = json.loads((ART / "feather.json").read_text(encoding="utf-8"))
    problems = []

    def expect(what: str, have: object, want: object) -> None:
        if have != want:
            problems.append(
                f"{what}: the art has {have!r}, the prototype has {want!r}"
            )

    drawn = [(n, p) for n, p in proto["glyphs"] if p]
    expect("glyphs", [(g["name"], g["path"]) for g in icons["glyphs"]], drawn)
    expect(
        "glyphs without a path (drawn by featherSvg)",
        [n for n, p in proto["glyphs"] if not p],
        [FEATHER_NAME],
    )
    side = str(icons["grid"])
    expect("grid", [side, side], list(proto["grid"]))
    expect(
        "glyph stroke width",
        float(icons["stroke_width"]),
        float(proto["stroke_width"]),
    )

    side = str(feather["view_box"])
    expect("feather view box", [side, side], list(proto["feather_box"]))
    items = feather["items"]
    expect(
        "feather vane",
        (items[0]["kind"], items[0]["paint"], items[0]["path"]),
        ("fill", "current", proto["vane"]),
    )
    expect(
        "feather vane is filled with currentColor", True, proto["feather_fill"]
    )
    expect(
        "feather strokes",
        [(str(float(i["width"])), i["path"]) for i in items[1:]],
        [(str(float(w)), d) for w, d in proto["feather_strokes"]],
    )
    wallpaper = json.loads(
        (ART / "wallpaper.json").read_text(encoding="utf-8")
    )
    width, height, body = proto["wallpaper"]
    expect(
        "wallpaper view box",
        [wallpaper["width"], wallpaper["height"]],
        [int(width), int(height)],
    )
    # Classes in the prototype's paint order -> the slot that carries each one.
    slot_of = {
        "f": "slot0",
        "r": "slot1",
        "a": "slot2",
        "b": "slot3",
        "c": "slot4",
    }
    expect(
        "wallpaper paths",
        [(i["paint"], i["path"]) for i in wallpaper["items"]],
        [
            (slot_of[c], d)
            for c, d in re.findall(r'<path class="(\w)" d="([^"]*)"', body)
        ],
    )
    return problems


def main() -> int:
    mode = sys.argv[1] if len(sys.argv) == 2 else ""
    if mode == "files":
        problems = check_files()
        summary = "art files ok: checksums, parse, every name exists"
    elif mode == "prototype":
        problems = check_prototype()
        summary = "art ok: the art sources equal the prototype"
    else:
        print(__doc__)
        return 2
    for problem in problems:
        print(f"FAIL {problem}")
    if problems:
        return 1
    print(summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
