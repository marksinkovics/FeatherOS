#!/usr/bin/env python3
"""Checks the committed font files without freetype-py: SHA256SUMS, every
FFNT file parsed, the licence files, and console_font.c against mono-16."""

from __future__ import annotations

import hashlib
import re
import sys
from pathlib import Path

import ffnt

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
FONTS = ROOT / "initramfs" / "fonts"
CONSOLE_C = ROOT / "src/kernel/framebuffer/console_font.c"
SOURCES = HERE / "sources"

LICENCES = {
    "LICENSE-JetBrainsMono.txt": "OFL-JetBrainsMono.txt",
    "LICENSE-Inter.txt": "OFL-Inter.txt",
}
EXPECTED = {
    "mono": (True, (16, 20, 24, 32)),
    "sans": (False, (16, 20, 24, 32, 48, 64)),
}
RESERVED_NAMES = ("jetbrains", "inter")


def check_sample() -> list[str]:
    path = FONTS / "sample.txt"
    if not path.exists():
        return ["sample.txt is missing"]
    if path.read_bytes() != (HERE / "sample.txt").read_bytes():
        return ["sample.txt differs from tools/fonts/sample.txt"]
    mono = FONTS / "mono-16.ffnt"
    if not mono.exists():
        return []
    have = {glyph.code_point for glyph in ffnt.read(mono.read_bytes()).glyphs}
    problems = []
    for number, line in enumerate(path.read_bytes().split(b"\n"), 1):
        text = line.decode("utf-8", errors="replace")
        lacking = sorted(
            {
                c
                for c in text
                if ord(c) not in have and not 0x80 <= ord(c) <= 0x9F
            }
        )
        if lacking:
            problems.append(f"sample.txt line {number} has {lacking!r}")
    return problems


def check() -> list[str]:
    problems = []
    for name, original in LICENCES.items():
        path = FONTS / name
        if not path.exists():
            problems.append(f"{name} is missing")
        elif path.read_bytes() != (SOURCES / original).read_bytes():
            problems.append(f"{name} differs from sources/{original}")
    if not (FONTS / "README.txt").exists():
        problems.append("README.txt is missing")
    problems += check_sample()

    sums = FONTS / "SHA256SUMS"
    listed = {}
    if not sums.exists():
        problems.append("SHA256SUMS is missing")
    else:
        for line in sums.read_text(encoding="utf-8").splitlines():
            digest, _, name = line.partition("  ")
            listed[name] = digest
    others = sorted(p.name for p in FONTS.iterdir() if p.name != "SHA256SUMS")
    if sorted(listed) != others:
        problems.append("SHA256SUMS does not list exactly the other files")
    for name, digest in listed.items():
        path = FONTS / name
        if path.exists() and hashlib.sha256(path.read_bytes()).hexdigest() != (
            digest
        ):
            problems.append(f"{name} does not match SHA256SUMS")

    for family, (monospace, sizes) in EXPECTED.items():
        for size in sizes:
            name = f"{family}-{size}.ffnt"
            path = FONTS / name
            if not path.exists():
                problems.append(f"{name} is missing")
                continue
            try:
                font = ffnt.read(path.read_bytes())
            except ffnt.FfntError as error:
                problems.append(f"{name} is refused: {error.status}")
                continue
            if font.name != f"{family} {size}":
                problems.append(f"{name} is named {font.name!r}")
            if any(word in font.name.lower() for word in RESERVED_NAMES):
                problems.append(f"{name} has a family name in its name")
            if font.monospace != monospace or font.line_height != size:
                problems.append(f"{name} has the wrong metrics")
            if monospace and font.cell_width != size // 2:
                problems.append(f"{name} has the wrong cell width")
            if "SIL Open Font License 1.1" not in font.notice:
                problems.append(f"{name} has no licence in its notice")
    extra = [
        p.name
        for p in FONTS.glob("*.ffnt")
        if not re.fullmatch(r"(mono|sans)-\d+\.ffnt", p.name)
    ]
    if extra:
        problems.append(f"unexpected font files {extra}")

    source = CONSOLE_C.read_text(encoding="utf-8")
    bytes_in_c = bytes(
        int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})", source)
    )
    mono = FONTS / "mono-16.ffnt"
    if mono.exists() and bytes_in_c != mono.read_bytes():
        problems.append("console_font.c differs from mono-16.ffnt")
    return problems


def main() -> int:
    problems = check()
    for problem in problems:
        print(f"check.py: {problem}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
