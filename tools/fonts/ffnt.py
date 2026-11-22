"""FFNT font files: reader, writer and the reference renderer."""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

MAGIC = b"FFNT"
VERSION = 1
HEADER_SIZE = 64
RECORD_SIZE = 20
COVERAGE_BITS = 4
MAXIMUM_FILE_SIZE = 4 * 1024 * 1024
MAXIMUM_BITMAP_SIDE = 256
CHECKSUM_OFFSET = 56
FLAG_MONOSPACE = 1

FNV64_OFFSET = 0xCBF29CE484222325
FNV64_PRIME = 0x100000001B3
FNV64_MASK = (1 << 64) - 1


class FfntError(Exception):
    def __init__(self, status: str) -> None:
        super().__init__(status)
        self.status = status


@dataclass
class Glyph:
    code_point: int
    left: int
    top: int
    advance: int
    rows: list[bytes] = field(default_factory=list)

    @property
    def width(self) -> int:
        return len(self.rows[0]) if self.rows else 0

    @property
    def height(self) -> int:
        return len(self.rows)


@dataclass
class Font:
    name: str
    notice: str
    monospace: bool
    em: int
    line_height: int
    ascent: int
    descent: int
    cell_width: int
    glyphs: list[Glyph] = field(default_factory=list)

    def find(self, code_point: int) -> Glyph:
        for glyph in self.glyphs:
            if glyph.code_point == code_point:
                return glyph
        return self.glyphs[0]


def fnv1a64(data: bytes) -> int:
    value = FNV64_OFFSET
    for byte in data:
        value = ((value ^ byte) * FNV64_PRIME) & FNV64_MASK
    return value


def file_checksum(data: bytes) -> int:
    zeroed = bytearray(data)
    zeroed[CHECKSUM_OFFSET : CHECKSUM_OFFSET + 8] = bytes(8)
    return fnv1a64(bytes(zeroed))


def pack_rows(rows: list[bytes]) -> bytes:
    out = bytearray()
    for row in rows:
        for x in range(0, len(row), 2):
            high = row[x]
            low = row[x + 1] if x + 1 < len(row) else 0
            out.append((high << 4) | low)
    return bytes(out)


def align4(value: int) -> int:
    return (value + 3) & ~3


def write(font: Font) -> bytes:
    table = bytearray()
    bitmaps = bytearray()
    for glyph in font.glyphs:
        packed = pack_rows(glyph.rows)
        table += struct.pack(
            "<IIHHhhHH",
            glyph.code_point,
            len(bitmaps),
            glyph.width,
            glyph.height,
            glyph.left,
            glyph.top,
            glyph.advance,
            0,
        )
        bitmaps += packed
    text = (
        font.name.encode("utf-8") + b"\0" + font.notice.encode("utf-8") + b"\0"
    )
    table_offset = HEADER_SIZE
    bitmaps_offset = table_offset + len(table)
    text_offset = align4(bitmaps_offset + len(bitmaps))
    size = text_offset + len(text)
    flags = FLAG_MONOSPACE if font.monospace else 0
    header = struct.pack(
        "<4sHHIIHHhhHBBI6I",
        MAGIC,
        VERSION,
        HEADER_SIZE,
        size,
        flags,
        font.em,
        font.line_height,
        font.ascent,
        font.descent,
        font.cell_width,
        COVERAGE_BITS,
        0,
        len(font.glyphs),
        table_offset,
        len(table),
        bitmaps_offset,
        len(bitmaps),
        text_offset,
        len(text),
    )
    data = bytearray(header + bytes(8))
    data += table
    data += bitmaps
    data += bytes(text_offset - len(data))
    data += text
    assert len(data) == size
    data[CHECKSUM_OFFSET : CHECKSUM_OFFSET + 8] = struct.pack(
        "<Q", file_checksum(bytes(data))
    )
    return bytes(data)


def valid_utf8(data: bytes) -> bool:
    try:
        data.decode("utf-8", errors="strict")
    except UnicodeDecodeError:
        return False
    return True


def read(data: bytes) -> Font:
    """Parses a file with the same refusals, in the same order, as the
    kernel's FontView::Parse."""
    if len(data) > MAXIMUM_FILE_SIZE:
        raise FfntError("size")
    if len(data) < HEADER_SIZE:
        raise FfntError("short")
    (
        magic,
        version,
        header_size,
        size,
        flags,
        em,
        line_height,
        ascent,
        descent,
        cell_width,
        coverage_bits,
        reserved,
        count,
        table_offset,
        table_size,
        bitmaps_offset,
        bitmaps_size,
        text_offset,
        text_size,
    ) = struct.unpack_from("<4sHHIIHHhhHBBI6I", data)
    (checksum,) = struct.unpack_from("<Q", data, CHECKSUM_OFFSET)
    if magic != MAGIC:
        raise FfntError("magic")
    if version != VERSION:
        raise FfntError("version")
    if (
        header_size != HEADER_SIZE
        or coverage_bits != COVERAGE_BITS
        or flags & ~FLAG_MONOSPACE
    ):
        raise FfntError("header")
    if reserved != 0:
        raise FfntError("reserved")
    if size != len(data):
        raise FfntError("length")
    if checksum != file_checksum(data):
        raise FfntError("checksum")
    offsets = (table_offset, bitmaps_offset, text_offset)
    if (
        any(offset % 4 for offset in offsets)
        or table_offset < HEADER_SIZE
        or table_size != count * RECORD_SIZE
        or table_offset + table_size > bitmaps_offset
        or bitmaps_offset + bitmaps_size > text_offset
        or text_offset + text_size > size
    ):
        raise FfntError("layout")
    records = [
        struct.unpack_from("<IIHHhhHH", data, table_offset + i * RECORD_SIZE)
        for i in range(count)
    ]
    for _, offset, width, height, _, _, _, spare in records:
        if spare != 0:
            raise FfntError("reserved")
        stride = (width + 1) // 2
        end = offset + height * stride
        if width % 2 and end <= bitmaps_size:
            for y in range(height):
                if (
                    data[bitmaps_offset + offset + y * stride + stride - 1]
                    & 15
                ):
                    raise FfntError("reserved")
    for previous, current in zip(records, records[1:]):
        if current[0] <= previous[0]:
            raise FfntError("order")
    if (
        em == 0
        or line_height == 0
        or ascent < 0
        or descent < 0
        or ascent + descent > line_height
        or (cell_width == 0) == bool(flags & FLAG_MONOSPACE)
    ):
        raise FfntError("range")
    limit = 4 * line_height
    for code_point, offset, width, height, left, top, advance, _ in records:
        stride = (width + 1) // 2
        if (
            code_point > 0x10FFFF
            or 0xD800 <= code_point <= 0xDFFF
            or offset + height * stride > bitmaps_size
            or width > MAXIMUM_BITMAP_SIDE
            or height > MAXIMUM_BITMAP_SIDE
            or abs(left) > limit
            or abs(top) > limit
            or advance > limit
            or (flags & FLAG_MONOSPACE and advance != cell_width)
        ):
            raise FfntError("range")
    if count == 0 or records[0][0] != 0:
        raise FfntError("missing")
    text = data[text_offset : text_offset + text_size]
    strings = text.split(b"\0")
    if (
        len(strings) < 3
        or any(strings[2:])
        or not valid_utf8(strings[0])
        or not valid_utf8(strings[1])
    ):
        raise FfntError("text")
    glyphs = []
    for code_point, offset, width, height, left, top, advance, _ in records:
        stride = (width + 1) // 2
        rows = []
        for y in range(height):
            start = bitmaps_offset + offset + y * stride
            row = bytearray()
            for x in range(width):
                byte = data[start + x // 2]
                row.append(byte & 15 if x % 2 else byte >> 4)
            rows.append(bytes(row))
        glyphs.append(Glyph(code_point, left, top, advance, rows))
    return Font(
        strings[0].decode("utf-8"),
        strings[1].decode("utf-8"),
        bool(flags & FLAG_MONOSPACE),
        em,
        line_height,
        ascent,
        descent,
        cell_width,
        glyphs,
    )


def blend(background: int, foreground: int, coverage: int) -> int:
    alpha = coverage * 17
    result = 0
    for shift in (16, 8, 0):
        b = (background >> shift) & 255
        f = (foreground >> shift) & 255
        result |= ((f * alpha + b * (255 - alpha) + 127) // 255) << shift
    return result


def is_control(code_point: int) -> bool:
    return code_point < 0x20 or code_point == 0x7F


def measure(font: Font, text: str) -> int:
    return sum(
        font.find(ord(c)).advance for c in text if not is_control(ord(c))
    )


def render_text(
    font: Font, text: str, background: int, foreground: int, margin: int = 4
) -> tuple[int, int, list[int]]:
    width = measure(font, text) + 2 * margin
    height = font.line_height + 2 * margin
    pixels = [background] * (width * height)
    pen = margin
    baseline = margin + font.ascent
    for char in text:
        code_point = ord(char)
        if is_control(code_point):
            continue
        glyph = font.find(code_point)
        for y, row in enumerate(glyph.rows):
            py = baseline - glyph.top + y
            if not 0 <= py < height:
                continue
            for x, coverage in enumerate(row):
                px = pen + glyph.left + x
                if coverage and 0 <= px < width:
                    index = py * width + px
                    pixels[index] = blend(pixels[index], foreground, coverage)
        pen += glyph.advance
    return width, height, pixels


def checksum32(pixels: list[int]) -> int:
    value = 2166136261
    for pixel in pixels:
        pixel &= 0xFFFFFF
        for shift in (0, 8, 16, 24):
            value = (
                (value ^ ((pixel >> shift) & 255)) * 16777619
            ) & 0xFFFFFFFF
    return value
