#!/usr/bin/env python3
"""Checks the pointers (CTest gui.pointers, docs/design/M37.md T18).

pointers_check.py --guirender EXE --system DIR [--report]

Renders every pointer at 24, 32 and 48 px with guirender and compares it with
the design's PNG in docs/design/gui/cursors/png/. Both are premultiplied RGBA;
the PNG is decoded with zlib alone. The mean and the largest absolute
difference per channel must stay within LIMITS, which were set just above what
was measured (design section 7). --report prints every measurement and fails
nothing.
"""

from __future__ import annotations

import argparse
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
PNG_DIR = ROOT / "docs" / "design" / "gui" / "cursors" / "png"

POINTERS = (
    "default",
    "pointer",
    "text",
    "progress",
    "wait",
    "grab",
    "grabbing",
    "ns",
    "ew",
    "nwse",
    "nesw",
    "no",
    "cross",
)
SIZES = (24, 32, 48)
ACCENT = "f0a840"
CHANNELS = "RGBA"

# size -> (limit of the mean, limit of the largest) absolute difference.
LIMITS: dict[int, tuple[float, int]] = {
    24: (9.0, 140),
    32: (3.6, 70),
    48: (2.7, 135),
}


class ImageError(Exception):
    pass


def paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def read_png(data: bytes) -> tuple[int, int, bytes]:
    """(width, height, RGBA bytes with straight alpha) of an 8-bit
    non-interlaced RGBA PNG."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ImageError("not a PNG")
    position = 8
    header = None
    compressed = bytearray()
    while position + 8 <= len(data):
        length, kind = struct.unpack(">I4s", data[position : position + 8])
        body = data[position + 8 : position + 8 + length]
        position += 12 + length
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            compressed += body
        elif kind == b"IEND":
            break
    if header is None:
        raise ImageError("no IHDR")
    width, height, depth, colour, _, _, interlace = header
    if (depth, colour, interlace) != (8, 6, 0):
        raise ImageError("only 8-bit non-interlaced RGBA is supported")
    raw = zlib.decompress(bytes(compressed))
    stride = width * 4
    if len(raw) != height * (stride + 1):
        raise ImageError("the pixel data has the wrong length")
    out = bytearray()
    previous = bytearray(stride)
    for row in range(height):
        start = row * (stride + 1)
        kind = raw[start]
        line = bytearray(raw[start + 1 : start + 1 + stride])
        for i in range(stride):
            left = line[i - 4] if i >= 4 else 0
            up = previous[i]
            upper_left = previous[i - 4] if i >= 4 else 0
            if kind == 0:
                delta = 0
            elif kind == 1:
                delta = left
            elif kind == 2:
                delta = up
            elif kind == 3:
                delta = (left + up) // 2
            elif kind == 4:
                delta = paeth(left, up, upper_left)
            else:
                raise ImageError(f"unknown filter {kind}")
            line[i] = (line[i] + delta) & 255
        out += line
        previous = line
    return width, height, bytes(out)


def premultiply(rgba: bytes) -> bytes:
    out = bytearray(len(rgba))
    for i in range(0, len(rgba), 4):
        alpha = rgba[i + 3]
        for c in range(3):
            out[i + c] = (rgba[i + c] * alpha + 127) // 255
        out[i + 3] = alpha
    return bytes(out)


def read_pam(data: bytes) -> tuple[int, int, bytes]:
    end = data.index(b"ENDHDR\n") + 7
    fields = dict(
        line.split(b" ", 1) for line in data[:end].splitlines() if b" " in line
    )
    width, height = int(fields[b"WIDTH"]), int(fields[b"HEIGHT"])
    if int(fields[b"DEPTH"]) != 4 or int(fields[b"MAXVAL"]) != 255:
        raise ImageError("guirender wrote an unexpected PAM")
    pixels = data[end:]
    if len(pixels) != width * height * 4:
        raise ImageError("the PAM has the wrong length")
    return width, height, pixels


def render(guirender: str, system: str, name: str, size: int) -> bytes:
    with tempfile.TemporaryDirectory() as folder:
        out = Path(folder) / "pointer.pam"
        subprocess.run(
            [
                guirender,
                "pointer",
                "--name",
                name,
                "--size",
                str(size),
                "--accent",
                ACCENT,
                "--system",
                system,
                "--out",
                str(out),
            ],
            check=True,
        )
        width, height, pixels = read_pam(out.read_bytes())
    if (width, height) != (size, size):
        raise ImageError(f"{name} at {size}: rendered {width}x{height}")
    return pixels


def measure(
    ours: bytes, reference: bytes
) -> tuple[list[float], list[int], int]:
    """Per channel mean and largest absolute difference, and the number of
    pixels that differ by more than 64 in any channel."""
    count = len(ours) // 4
    sums = [0, 0, 0, 0]
    largest = [0, 0, 0, 0]
    far = 0
    for i in range(0, len(ours), 4):
        worst = 0
        for c in range(4):
            difference = abs(ours[i + c] - reference[i + c])
            sums[c] += difference
            largest[c] = max(largest[c], difference)
            worst = max(worst, difference)
        far += worst > 64
    return [s / count for s in sums], largest, far


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--guirender", required=True)
    parser.add_argument("--system", required=True)
    parser.add_argument("--report", action="store_true")
    args = parser.parse_args()
    failures = []
    worst: dict[int, tuple[float, int]] = {size: (0.0, 0) for size in SIZES}
    for size in SIZES:
        for name in POINTERS:
            try:
                width, height, straight = read_png(
                    (PNG_DIR / f"{name}-{size}.png").read_bytes()
                )
                if (width, height) != (size, size):
                    raise ImageError(f"{name}-{size}.png is {width}x{height}")
                ours = render(args.guirender, args.system, name, size)
            except (ImageError, OSError, subprocess.CalledProcessError) as e:
                failures.append(f"{name} at {size}: {e}")
                continue
            means, largest, far = measure(ours, premultiply(straight))
            mean_limit, largest_limit = LIMITS[size]
            top_mean = max(means)
            top_largest = max(largest)
            worst[size] = (
                max(worst[size][0], top_mean),
                max(worst[size][1], top_largest),
            )
            if args.report:
                print(
                    f"{size:2d} {name:9s} mean "
                    + " ".join(f"{m:6.3f}" for m in means)
                    + " max "
                    + " ".join(f"{m:3d}" for m in largest)
                    + f" far {far}"
                )
            if top_mean > mean_limit or top_largest > largest_limit:
                failures.append(
                    f"{name} at {size}: mean {top_mean:.3f} (limit "
                    f"{mean_limit}), largest {top_largest} (limit "
                    f"{largest_limit})"
                )
    if args.report:
        for size in SIZES:
            print(
                f"{size}: worst mean {worst[size][0]:.3f}, "
                f"worst largest {worst[size][1]}"
            )
        return 0
    for failure in failures:
        print(f"FAIL {failure}")
    if failures:
        return 1
    print("pointers ok: 13 pointers at 24, 32 and 48 px within the limits")
    return 0


if __name__ == "__main__":
    sys.exit(main())
