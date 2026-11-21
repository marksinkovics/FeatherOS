#include "font_builder.h"

namespace FeatherOS::TestSupport
{

namespace
{

constexpr size_t HeaderBytes = 64;

void Append16(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void Append32(std::vector<uint8_t>& out, uint32_t value)
{
    Append16(out, static_cast<uint16_t>(value));
    Append16(out, static_cast<uint16_t>(value >> 16));
}

uint64_t Fnv1a(const std::vector<uint8_t>& bytes)
{
    uint64_t hash = 0xCBF29CE484222325ULL;
    for (size_t index = 0; index < bytes.size(); ++index)
    {
        const bool inside = index >= FontOffset::Checksum && index < FontOffset::Checksum + 8;
        hash = (hash ^ (inside ? 0 : bytes[index])) * 0x100000001B3ULL;
    }
    return hash;
}

TestGlyph MakeGlyph(uint32_t code_point, int width, int height)
{
    TestGlyph glyph;
    glyph.code_point = code_point;
    glyph.left = static_cast<int16_t>(code_point % 3);
    glyph.top = static_cast<int16_t>(1 + code_point % 5);
    glyph.advance = static_cast<uint16_t>(6 + code_point % 3);
    glyph.width = static_cast<uint16_t>(width);
    glyph.height = static_cast<uint16_t>(height);
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            glyph.coverage.push_back(SampleCoverage(code_point, x, y));
        }
    }
    return glyph;
}

} // namespace

uint8_t SampleCoverage(uint32_t code_point, int x, int y)
{
    return static_cast<uint8_t>(
        (code_point + 3 * static_cast<uint32_t>(x) + 5 * static_cast<uint32_t>(y)) % 16);
}

TestFont SampleFont()
{
    TestFont font;
    TestGlyph missing;
    missing.left = 1;
    missing.top = 6;
    missing.advance = 7;
    missing.width = 5;
    missing.height = 6;
    for (int y = 0; y < 6; ++y)
    {
        for (int x = 0; x < 5; ++x)
        {
            missing.coverage.push_back((y == 0 || y == 5 || x == 0 || x == 4) ? 15 : 0);
        }
    }
    font.glyphs.push_back(missing);
    for (uint32_t code_point = 0x20; code_point <= 0x7E; ++code_point)
    {
        const bool space = code_point == 0x20;
        font.glyphs.push_back(MakeGlyph(code_point,
                                        space ? 0 : static_cast<int>(1 + code_point % 5),
                                        space ? 0 : static_cast<int>(1 + code_point % 4)));
    }
    TestGlyph written;
    written.code_point = 0xA0;
    written.left = -1;
    written.top = 2;
    written.advance = 5;
    written.width = 3;
    written.height = 2;
    written.coverage = {1, 2, 3, 15, 0, 7};
    font.glyphs.push_back(written);
    font.glyphs.push_back(MakeGlyph(0xE9, 5, 5));
    font.glyphs.push_back(MakeGlyph(0x391, 4, 4));
    font.glyphs.push_back(MakeGlyph(0x2500, 6, 2));
    font.glyphs.push_back(MakeGlyph(0x10FFFF, 2, 2));
    return font;
}

std::vector<uint8_t> BuildFont(const TestFont& font)
{
    std::vector<uint8_t> table;
    std::vector<uint8_t> bitmaps;
    for (const TestGlyph& glyph : font.glyphs)
    {
        Append32(table, glyph.code_point);
        Append32(table, static_cast<uint32_t>(bitmaps.size()));
        Append16(table, glyph.width);
        Append16(table, glyph.height);
        Append16(table, static_cast<uint16_t>(glyph.left));
        Append16(table, static_cast<uint16_t>(glyph.top));
        Append16(table, glyph.advance);
        Append16(table, glyph.spare);
        for (int y = 0; y < glyph.height; ++y)
        {
            for (int x = 0; x < glyph.width; x += 2)
            {
                const uint8_t high = glyph.coverage[static_cast<size_t>(y * glyph.width + x)];
                const uint8_t low =
                    x + 1 < glyph.width
                        ? glyph.coverage[static_cast<size_t>(y * glyph.width + x + 1)]
                        : 0;
                bitmaps.push_back(static_cast<uint8_t>((high << 4) | low));
            }
        }
    }
    std::vector<uint8_t> text(font.name.begin(), font.name.end());
    text.push_back(0);
    text.insert(text.end(), font.notice.begin(), font.notice.end());
    text.push_back(0);
    text.insert(text.end(), font.text_suffix.begin(), font.text_suffix.end());

    const size_t table_offset = HeaderBytes;
    const size_t bitmaps_offset = table_offset + table.size();
    const size_t text_offset = (bitmaps_offset + bitmaps.size() + 3) & ~static_cast<size_t>(3);
    const size_t size = text_offset + text.size();

    std::vector<uint8_t> out;
    out.insert(out.end(), {'F', 'F', 'N', 'T'});
    Append16(out, 1);
    Append16(out, HeaderBytes);
    Append32(out, static_cast<uint32_t>(size));
    Append32(out, font.monospace ? 1U : 0U);
    Append16(out, font.em);
    Append16(out, font.line_height);
    Append16(out, static_cast<uint16_t>(font.ascent));
    Append16(out, static_cast<uint16_t>(font.descent));
    Append16(out, font.cell_width);
    out.push_back(4);
    out.push_back(0);
    Append32(out, static_cast<uint32_t>(font.glyphs.size()));
    Append32(out, static_cast<uint32_t>(table_offset));
    Append32(out, static_cast<uint32_t>(table.size()));
    Append32(out, static_cast<uint32_t>(bitmaps_offset));
    Append32(out, static_cast<uint32_t>(bitmaps.size()));
    Append32(out, static_cast<uint32_t>(text_offset));
    Append32(out, static_cast<uint32_t>(text.size()));
    out.resize(HeaderBytes, 0);
    out.insert(out.end(), table.begin(), table.end());
    out.insert(out.end(), bitmaps.begin(), bitmaps.end());
    out.resize(text_offset, 0);
    out.insert(out.end(), text.begin(), text.end());
    Seal(out);
    return out;
}

void Seal(std::vector<uint8_t>& bytes)
{
    const uint64_t hash = Fnv1a(bytes);
    for (size_t index = 0; index < 8; ++index)
    {
        bytes[FontOffset::Checksum + index] = static_cast<uint8_t>(hash >> (8 * index));
    }
}

uint16_t Get16(const std::vector<uint8_t>& bytes, size_t offset)
{
    return static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

uint32_t Get32(const std::vector<uint8_t>& bytes, size_t offset)
{
    return static_cast<uint32_t>(Get16(bytes, offset)) |
           (static_cast<uint32_t>(Get16(bytes, offset + 2)) << 16);
}

void Put16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value)
{
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void Put32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value)
{
    Put16(bytes, offset, static_cast<uint16_t>(value));
    Put16(bytes, offset + 2, static_cast<uint16_t>(value >> 16));
}

size_t RecordOffset(const std::vector<uint8_t>& bytes, size_t index)
{
    return Get32(bytes, FontOffset::TableOffset) + index * FontOffset::RecordSize;
}

size_t BitmapOffset(const std::vector<uint8_t>& bytes, size_t index)
{
    return Get32(bytes, FontOffset::BitmapsOffset) + Get32(bytes, RecordOffset(bytes, index) + 4);
}

} // namespace FeatherOS::TestSupport
