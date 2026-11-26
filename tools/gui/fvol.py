#!/usr/bin/env python3
"""Reader and writer of the FVOL outlines file (docs/design/M37.md 1.5).

The layout and every limit follow src/user/graphics/outlines.{h,cpp}, which
is the authority: this module writes exactly what that loader accepts and
refuses what it refuses, with the same reason names.

fvol.py FILE   print the drawings of a file
"""

from __future__ import annotations

import struct
import sys
from dataclasses import dataclass, field

MAGIC = b"FVOL"
VERSION = 1
HEADER_SIZE = 32
RECORD_SIZE = 64
ITEM_SIZE = 16
CHECKSUM_OFFSET = 24
NAME_BYTES = 32
MAXIMUM_NAME_LENGTH = 31
MAXIMUM_VIEW_BOX = 4096
MAXIMUM_FILE_SIZE = 4 << 20
MAXIMUM_DRAWING_POINTS = 8192
MAXIMUM_FILE_POINTS = 65536
SLOTS = 8
POINT_MARGIN = 256
UNIT = 256

CURRENT = 0
FIXED = 1
SLOT = 2

FNV64_OFFSET = 0xCBF29CE484222325
FNV64_PRIME = 0x100000001B3
FNV64_MASK = (1 << 64) - 1

FLAG_HOTSPOT = 1


class FvolError(Exception):
    """A damaged file or drawing; `status` is the loader's reason name."""

    def __init__(self, status: str, message: str = "") -> None:
        super().__init__(f"{status}: {message}" if message else status)
        self.status = status


@dataclass
class Polyline:
    points: list[tuple[int, int]]
    closed: bool = False


@dataclass
class Item:
    stroke: bool
    paint: int = CURRENT
    slot: int = 0
    color: int = 0
    opacity: int = 255
    width: int = 0
    polylines: list[Polyline] = field(default_factory=list)


@dataclass
class Drawing:
    name: str
    width: int
    height: int
    items: list[Item]
    hotspot: tuple[int, int] | None = None


def fnv1a64(data: bytes) -> int:
    value = FNV64_OFFSET
    for byte in data:
        value = ((value ^ byte) * FNV64_PRIME) & FNV64_MASK
    return value


def file_checksum(data: bytes) -> int:
    zeroed = bytearray(data)
    zeroed[CHECKSUM_OFFSET : CHECKSUM_OFFSET + 8] = bytes(8)
    return fnv1a64(bytes(zeroed))


def in_range(value: int, extent: int) -> bool:
    return -extent * POINT_MARGIN <= value <= 2 * extent * POINT_MARGIN


def encode_name(name: str) -> bytes:
    raw = name.encode("ascii", errors="replace")
    if (
        not raw
        or len(raw) > MAXIMUM_NAME_LENGTH
        or any(not 0x21 <= byte <= 0x7E for byte in raw)
    ):
        raise FvolError("name", repr(name))
    return raw.ljust(NAME_BYTES, b"\0")


def encode_drawing(drawing: Drawing) -> tuple[bytes, int]:
    """The drawing's data bytes and its point count."""
    out = bytearray()
    points = 0
    for item in drawing.items:
        out += struct.pack(
            "<BBBBIiHH",
            1 if item.stroke else 0,
            item.paint,
            item.slot,
            item.opacity,
            item.color,
            item.width,
            len(item.polylines),
            0,
        )
        for polyline in item.polylines:
            out += struct.pack(
                "<HBB", len(polyline.points), 1 if polyline.closed else 0, 0
            )
            for x, y in polyline.points:
                out += struct.pack("<ii", x, y)
            points += len(polyline.points)
    return bytes(out), points


def write(drawings: list[Drawing]) -> bytes:
    """The file for `drawings`, sorted by name. Refuses whatever the loader
    would, by reading the result back."""
    ordered = sorted(drawings, key=lambda d: encode_name(d.name))
    data = bytearray()
    records = bytearray()
    offset = HEADER_SIZE + RECORD_SIZE * len(ordered)
    for drawing in ordered:
        body, _ = encode_drawing(drawing)
        flags = FLAG_HOTSPOT if drawing.hotspot is not None else 0
        hotspot_x, hotspot_y = drawing.hotspot or (0, 0)
        records += encode_name(drawing.name) + struct.pack(
            "<IIIiiIII",
            drawing.width,
            drawing.height,
            flags,
            hotspot_x,
            hotspot_y,
            offset + len(data),
            len(body),
            len(drawing.items),
        )
        data += body
    size = HEADER_SIZE + len(records) + len(data)
    header = (
        MAGIC
        + struct.pack("<HHII", VERSION, HEADER_SIZE, size, len(ordered))
        + bytes(8)
        + bytes(8)
    )
    blob = bytearray(header + records + data)
    blob[CHECKSUM_OFFSET : CHECKSUM_OFFSET + 8] = struct.pack(
        "<Q", file_checksum(bytes(blob))
    )
    read(bytes(blob))
    return bytes(blob)


def read(data: bytes) -> list[Drawing]:
    """Parses a file with the loader's checks; raises FvolError."""
    size = len(data)
    if size > MAXIMUM_FILE_SIZE:
        raise FvolError("size")
    if size < HEADER_SIZE:
        raise FvolError("short")
    if data[:4] != MAGIC:
        raise FvolError("magic")
    version, header_size, length, count = struct.unpack_from("<HHII", data, 4)
    if version != VERSION:
        raise FvolError("version")
    if header_size != HEADER_SIZE or data[16:24] != bytes(8):
        raise FvolError("header")
    if length != size:
        raise FvolError("length")
    (checksum,) = struct.unpack_from("<Q", data, CHECKSUM_OFFSET)
    if checksum != file_checksum(data):
        raise FvolError("checksum")
    if count == 0 or (size - HEADER_SIZE) // RECORD_SIZE < count:
        raise FvolError("layout")
    expected = HEADER_SIZE + count * RECORD_SIZE
    drawings = []
    previous = None
    file_points = 0
    for index in range(count):
        record = data[
            HEADER_SIZE + index * RECORD_SIZE : HEADER_SIZE
            + (index + 1) * RECORD_SIZE
        ]
        name = check_name(record[:NAME_BYTES])
        if previous is not None and previous >= record[:NAME_BYTES]:
            raise FvolError("order", name)
        previous = record[:NAME_BYTES]
        drawing, points = read_drawing(data, record, name, expected, size)
        file_points += points
        if file_points > MAXIMUM_FILE_POINTS:
            raise FvolError("points", name)
        expected += len(encode_drawing(drawing)[0])
        drawings.append(drawing)
    if expected != size:
        raise FvolError("layout")
    return drawings


def check_name(raw: bytes) -> str:
    length = 0
    while length < NAME_BYTES and raw[length] != 0:
        if not 0x21 <= raw[length] <= 0x7E:
            raise FvolError("name")
        length += 1
    if length == 0 or length > MAXIMUM_NAME_LENGTH or any(raw[length:]):
        raise FvolError("name")
    return raw[:length].decode("ascii")


def read_drawing(
    data: bytes, record: bytes, name: str, expected: int, size: int
) -> tuple[Drawing, int]:
    (
        width,
        height,
        flags,
        hotspot_x,
        hotspot_y,
        offset,
        data_size,
        item_count,
    ) = struct.unpack_from("<IIIiiIII", record, NAME_BYTES)
    if not 1 <= width <= MAXIMUM_VIEW_BOX or not 1 <= height <= (
        MAXIMUM_VIEW_BOX
    ):
        raise FvolError("range", name)
    has_hotspot = bool(flags & FLAG_HOTSPOT)
    if (
        flags & ~FLAG_HOTSPOT
        or (not has_hotspot and (hotspot_x or hotspot_y))
        or offset != expected
        or data_size > size - expected
    ):
        raise FvolError("layout", name)
    if has_hotspot and not (
        in_range(hotspot_x, width) and in_range(hotspot_y, height)
    ):
        raise FvolError("range", name)
    body = data[offset : offset + data_size]
    items, points = read_items(body, name, width, height, item_count)
    drawing = Drawing(
        name,
        width,
        height,
        items,
        (hotspot_x, hotspot_y) if has_hotspot else None,
    )
    return drawing, points


def read_items(
    body: bytes, name: str, width: int, height: int, item_count: int
) -> tuple[list[Item], int]:
    if item_count == 0:
        raise FvolError("item", name)
    widest = 2 * POINT_MARGIN * min(width, height)
    position = 0
    points = 0
    items = []
    for _ in range(item_count):
        if len(body) - position < ITEM_SIZE:
            raise FvolError("item", name)
        kind, paint, slot, opacity, color, stroke_width, polylines, zero = (
            struct.unpack_from("<BBBBIiHH", body, position)
        )
        position += ITEM_SIZE
        if kind > 1:
            raise FvolError("item", name)
        if (
            paint > SLOT
            or slot >= SLOTS
            or (paint != SLOT and slot != 0)
            or (paint != FIXED and color != 0)
        ):
            raise FvolError("paint", name)
        if kind == 1:
            if not 0 <= stroke_width <= widest:
                raise FvolError("range", name)
        elif stroke_width != 0:
            raise FvolError("item", name)
        if polylines == 0 or zero != 0:
            raise FvolError("item", name)
        item = Item(kind == 1, paint, slot, color, opacity, stroke_width)
        for _ in range(polylines):
            if len(body) - position < 4:
                raise FvolError("item", name)
            count, closed, zero = struct.unpack_from("<HBB", body, position)
            position += 4
            if count < 2 or closed > 1 or zero != 0:
                raise FvolError("polyline", name)
            if (len(body) - position) // 8 < count:
                raise FvolError("item", name)
            points += count
            if points > MAXIMUM_DRAWING_POINTS:
                raise FvolError("points", name)
            coordinates = struct.unpack_from(f"<{2 * count}i", body, position)
            polyline = Polyline(
                list(zip(coordinates[0::2], coordinates[1::2])), bool(closed)
            )
            for x, y in polyline.points:
                if not (in_range(x, width) and in_range(y, height)):
                    raise FvolError("range", name)
            position += count * 8
            item.polylines.append(polyline)
        items.append(item)
    if position != len(body):
        raise FvolError("item", name)
    return items, points


def describe(drawings: list[Drawing]) -> list[str]:
    lines = []
    for drawing in drawings:
        points = sum(
            len(p.points) for item in drawing.items for p in item.polylines
        )
        lines.append(
            f"{drawing.name}: {drawing.width}x{drawing.height}, "
            f"{len(drawing.items)} items, {points} points"
        )
    return lines


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    try:
        with open(sys.argv[1], "rb") as handle:
            drawings = read(handle.read())
    except (OSError, FvolError) as error:
        print(f"FAIL {error}")
        return 1
    print("\n".join(describe(drawings)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
