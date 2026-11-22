#include "font_data.h"

#include "utf8.h"

namespace FeatherOS::Text
{

namespace
{

constexpr uint16_t Version = 1;
constexpr size_t ChecksumOffset = 56;
constexpr uint32_t FlagMonospace = 1;
constexpr uint8_t CoverageBits = 4;
constexpr uint32_t MaximumBitmapSide = 256;
constexpr uint32_t DenseFirst = 0x20;
constexpr uint32_t DenseCount = 95;
constexpr char32_t LastCodePoint = 0x10FFFF;

uint16_t Le16(const uint8_t* bytes)
{
    return static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
}

uint32_t Le32(const uint8_t* bytes)
{
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

uint64_t Checksum(const uint8_t* bytes, size_t size)
{
    uint64_t hash = 0xCBF29CE484222325ULL;
    for (size_t index = 0; index < size; ++index)
    {
        const bool inside = index >= ChecksumOffset && index < ChecksumOffset + 8;
        hash = (hash ^ (inside ? 0U : bytes[index])) * 0x100000001B3ULL;
    }
    return hash;
}

uint64_t Le64(const uint8_t* bytes)
{
    return static_cast<uint64_t>(Le32(bytes)) | (static_cast<uint64_t>(Le32(bytes + 4)) << 32);
}

Glyph ReadRecord(const uint8_t* table, uint32_t index)
{
    const uint8_t* record = table + static_cast<size_t>(index) * FontView::RecordSize;
    return {Le32(record),
            Le32(record + 4),
            Le16(record + 8),
            Le16(record + 10),
            static_cast<int16_t>(Le16(record + 12)),
            static_cast<int16_t>(Le16(record + 14)),
            Le16(record + 16)};
}

size_t Stride(uint32_t width)
{
    return (static_cast<size_t>(width) + 1) / 2;
}

struct Section
{
    uint64_t offset;
    uint64_t size;
};

// Both strings must end inside the section, be valid UTF-8, and leave only zero bytes.
bool ReadText(const uint8_t* bytes, const Section& section, const char** name, const char** notice)
{
    const char* text = reinterpret_cast<const char*>(bytes + section.offset);
    const size_t size = static_cast<size_t>(section.size);
    size_t first = 0;
    while (first < size && text[first] != '\0')
    {
        ++first;
    }
    if (first == size)
    {
        return false;
    }
    size_t second = first + 1;
    while (second < size && text[second] != '\0')
    {
        ++second;
    }
    if (second == size)
    {
        return false;
    }
    for (size_t index = second + 1; index < size; ++index)
    {
        if (text[index] != '\0')
        {
            return false;
        }
    }
    if (!IsValidUtf8(text, first) || !IsValidUtf8(text + first + 1, second - first - 1))
    {
        return false;
    }
    *name = text;
    *notice = text + first + 1;
    return true;
}

} // namespace

const char* FontStatusName(FontStatus status)
{
    switch (status)
    {
    case FontStatus::Ok:
        return "ok";
    case FontStatus::Size:
        return "size";
    case FontStatus::Short:
        return "short";
    case FontStatus::Magic:
        return "magic";
    case FontStatus::Version:
        return "version";
    case FontStatus::Header:
        return "header";
    case FontStatus::Reserved:
        return "reserved";
    case FontStatus::Length:
        return "length";
    case FontStatus::Checksum:
        return "checksum";
    case FontStatus::Layout:
        return "layout";
    case FontStatus::Order:
        return "order";
    case FontStatus::Range:
        return "range";
    case FontStatus::Missing:
        return "missing";
    case FontStatus::Text:
        return "text";
    }
    return "unknown";
}

FontStatus FontView::Parse(const uint8_t* bytes, size_t size, FontView* view)
{
    if (size > MaximumFileSize)
    {
        return FontStatus::Size;
    }
    if (size < HeaderSize)
    {
        return FontStatus::Short;
    }
    if (bytes[0] != 'F' || bytes[1] != 'F' || bytes[2] != 'N' || bytes[3] != 'T')
    {
        return FontStatus::Magic;
    }
    if (Le16(bytes + 4) != Version)
    {
        return FontStatus::Version;
    }
    const uint32_t flags = Le32(bytes + 12);
    if (Le16(bytes + 6) != HeaderSize || bytes[26] != CoverageBits || (flags & ~FlagMonospace) != 0)
    {
        return FontStatus::Header;
    }
    if (bytes[27] != 0)
    {
        return FontStatus::Reserved;
    }
    if (Le32(bytes + 8) != size)
    {
        return FontStatus::Length;
    }
    if (Le64(bytes + ChecksumOffset) != Checksum(bytes, size))
    {
        return FontStatus::Checksum;
    }

    const uint32_t count = Le32(bytes + 28);
    const Section table{Le32(bytes + 32), Le32(bytes + 36)};
    const Section bitmaps{Le32(bytes + 40), Le32(bytes + 44)};
    const Section text{Le32(bytes + 48), Le32(bytes + 52)};
    if (table.offset % 4 != 0 || bitmaps.offset % 4 != 0 || text.offset % 4 != 0 ||
        table.offset < HeaderSize || table.size != static_cast<uint64_t>(count) * RecordSize ||
        table.offset + table.size > bitmaps.offset || bitmaps.offset + bitmaps.size > text.offset ||
        text.offset + text.size > size)
    {
        return FontStatus::Layout;
    }
    const uint8_t* records = bytes + table.offset;
    const uint8_t* bitmap_bytes = bytes + bitmaps.offset;

    for (uint32_t index = 0; index < count; ++index)
    {
        const Glyph glyph = ReadRecord(records, index);
        if (Le16(records + static_cast<size_t>(index) * RecordSize + 18) != 0)
        {
            return FontStatus::Reserved;
        }
        const uint64_t stride = Stride(glyph.width);
        if ((glyph.width % 2) != 0 &&
            static_cast<uint64_t>(glyph.bitmap_offset) + glyph.height * stride <= bitmaps.size)
        {
            for (uint32_t row = 0; row < glyph.height; ++row)
            {
                const uint8_t last = bitmap_bytes[glyph.bitmap_offset + row * stride + stride - 1];
                if ((last & 0x0F) != 0)
                {
                    return FontStatus::Reserved;
                }
            }
        }
    }
    for (uint32_t index = 1; index < count; ++index)
    {
        if (ReadRecord(records, index).code_point <= ReadRecord(records, index - 1).code_point)
        {
            return FontStatus::Order;
        }
    }

    const uint16_t em = Le16(bytes + 16);
    const uint16_t line_height = Le16(bytes + 18);
    const int16_t ascent = static_cast<int16_t>(Le16(bytes + 20));
    const int16_t descent = static_cast<int16_t>(Le16(bytes + 22));
    const uint16_t cell_width = Le16(bytes + 24);
    const bool monospace = (flags & FlagMonospace) != 0;
    if (em == 0 || line_height == 0 || ascent < 0 || descent < 0 ||
        ascent + descent > line_height || (cell_width == 0) == monospace)
    {
        return FontStatus::Range;
    }
    const int32_t limit = 4 * line_height;
    for (uint32_t index = 0; index < count; ++index)
    {
        const Glyph glyph = ReadRecord(records, index);
        const bool surrogate = glyph.code_point >= 0xD800 && glyph.code_point <= 0xDFFF;
        if (glyph.code_point > LastCodePoint || surrogate ||
            static_cast<uint64_t>(glyph.bitmap_offset) + glyph.height * Stride(glyph.width) >
                bitmaps.size ||
            glyph.width > MaximumBitmapSide || glyph.height > MaximumBitmapSide ||
            glyph.left > limit || glyph.left < -limit || glyph.top > limit || glyph.top < -limit ||
            glyph.advance > limit || (monospace && glyph.advance != cell_width))
        {
            return FontStatus::Range;
        }
    }
    if (count == 0 || ReadRecord(records, 0).code_point != 0)
    {
        return FontStatus::Missing;
    }
    const char* name = nullptr;
    const char* notice = nullptr;
    if (!ReadText(bytes, text, &name, &notice))
    {
        return FontStatus::Text;
    }

    FontView parsed;
    parsed.m_table = records;
    parsed.m_bitmaps = bitmap_bytes;
    parsed.m_name = name;
    parsed.m_notice = notice;
    parsed.m_count = count;
    parsed.m_em = em;
    parsed.m_line_height = line_height;
    parsed.m_ascent = ascent;
    parsed.m_descent = descent;
    parsed.m_cell_width = cell_width;
    parsed.m_monospace = monospace;
    if (count > DenseCount && ReadRecord(records, 1).code_point == DenseFirst &&
        ReadRecord(records, DenseCount).code_point == DenseFirst + DenseCount - 1)
    {
        parsed.m_dense = 1;
    }
    *view = parsed;
    return FontStatus::Ok;
}

bool FontView::Valid() const
{
    return m_table != nullptr;
}

bool FontView::Monospace() const
{
    return m_monospace;
}

uint16_t FontView::EmSize() const
{
    return m_em;
}

uint16_t FontView::LineHeight() const
{
    return m_line_height;
}

int16_t FontView::Ascent() const
{
    return m_ascent;
}

int16_t FontView::Descent() const
{
    return m_descent;
}

uint16_t FontView::CellWidth() const
{
    return m_cell_width;
}

uint32_t FontView::GlyphCount() const
{
    return m_count;
}

const char* FontView::Name() const
{
    return m_name;
}

const char* FontView::Notice() const
{
    return m_notice;
}

Glyph FontView::GlyphAt(uint32_t index) const
{
    return ReadRecord(m_table, index);
}

bool FontView::Lookup(char32_t code_point, uint32_t* index) const
{
    if (m_dense != 0 && code_point >= DenseFirst && code_point < DenseFirst + DenseCount)
    {
        *index = m_dense + (code_point - DenseFirst);
        return true;
    }
    uint32_t low = 0;
    uint32_t high = m_count;
    while (low < high)
    {
        const uint32_t middle = low + (high - low) / 2;
        const char32_t found = ReadRecord(m_table, middle).code_point;
        if (found == code_point)
        {
            *index = middle;
            return true;
        }
        if (found < code_point)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    return false;
}

bool FontView::Contains(char32_t code_point) const
{
    uint32_t index = 0;
    return Lookup(code_point, &index);
}

Glyph FontView::Find(char32_t code_point) const
{
    uint32_t index = 0;
    return ReadRecord(m_table, Lookup(code_point, &index) ? index : 0);
}

uint8_t FontView::Coverage(const Glyph& glyph, int32_t x, int32_t y) const
{
    if (x < 0 || y < 0 || x >= glyph.width || y >= glyph.height)
    {
        return 0;
    }
    const uint8_t byte =
        m_bitmaps[glyph.bitmap_offset + static_cast<size_t>(y) * Stride(glyph.width) +
                  static_cast<size_t>(x) / 2];
    return (x % 2) != 0 ? (byte & 0x0F) : (byte >> 4);
}

const uint8_t* FontView::Bitmap(const Glyph& glyph) const
{
    return m_bitmaps + glyph.bitmap_offset;
}

size_t FontView::BitmapStride(uint32_t width)
{
    return Stride(width);
}

uint32_t Blend(uint32_t background, uint32_t foreground, uint32_t coverage)
{
    const uint32_t alpha = (coverage > 15 ? 15 : coverage) * 17;
    uint32_t result = 0;
    for (int shift = 16; shift >= 0; shift -= 8)
    {
        const uint32_t b = (background >> shift) & 0xFF;
        const uint32_t f = (foreground >> shift) & 0xFF;
        result |= ((f * alpha + b * (255 - alpha) + 127) / 255) << shift;
    }
    return result;
}

} // namespace FeatherOS::Text
