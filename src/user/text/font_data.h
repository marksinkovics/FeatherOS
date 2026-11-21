#ifndef FEATHEROS_USER_TEXT_FONT_DATA_H
#define FEATHEROS_USER_TEXT_FONT_DATA_H

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Text
{

enum class FontStatus : int32_t
{
    Ok = 0,
    Size,
    Short,
    Magic,
    Version,
    Header,
    Reserved,
    Length,
    Checksum,
    Layout,
    Order,
    Range,
    Missing,
    Text,
};

const char* FontStatusName(FontStatus status);

struct Glyph
{
    char32_t code_point;
    uint32_t bitmap_offset;
    uint16_t width;
    uint16_t height;
    int16_t left;
    int16_t top;
    uint16_t advance;
};

class FontView
{
public:
    static constexpr size_t MaximumFileSize = size_t{4} << 20;
    static constexpr size_t HeaderSize = 64;
    static constexpr size_t RecordSize = 20;

    static FontStatus Parse(const uint8_t* bytes, size_t size, FontView* view);

    bool Valid() const;
    bool Monospace() const;
    uint16_t EmSize() const;
    uint16_t LineHeight() const;
    int16_t Ascent() const;
    int16_t Descent() const;
    uint16_t CellWidth() const;
    uint32_t GlyphCount() const;
    const char* Name() const;
    const char* Notice() const;

    Glyph GlyphAt(uint32_t index) const;
    bool Contains(char32_t code_point) const;
    // Record 0, the missing-glyph box, for a code point the font lacks.
    Glyph Find(char32_t code_point) const;
    // Coverage 0 to 15; 0 outside the bitmap.
    uint8_t Coverage(const Glyph& glyph, int32_t x, int32_t y) const;

private:
    bool Lookup(char32_t code_point, uint32_t* index) const;

    const uint8_t* m_table = nullptr;
    const uint8_t* m_bitmaps = nullptr;
    const char* m_name = nullptr;
    const char* m_notice = nullptr;
    uint32_t m_count = 0;
    uint32_t m_dense = 0;
    uint16_t m_em = 0;
    uint16_t m_line_height = 0;
    int16_t m_ascent = 0;
    int16_t m_descent = 0;
    uint16_t m_cell_width = 0;
    bool m_monospace = false;
};

// Per channel, in sRGB with no gamma: (f * a + b * (255 - a) + 127) / 255 with a = coverage * 17.
uint32_t Blend(uint32_t background, uint32_t foreground, uint32_t coverage);

} // namespace FeatherOS::Text

#endif
