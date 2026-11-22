#include "gfx_check.h"

#include "../kernel/framebuffer/console_font.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

namespace FeatherOS::User::GfxCheck
{

namespace
{

constexpr int32_t OsX = 12;
constexpr int32_t OsY = 22;
constexpr int32_t SpriteX = 50;
constexpr int32_t SpriteY = 30;

uint32_t Picture[Width * Height];
uint32_t TilePicture[TilePictureWidth * TilePictureHeight];
uint32_t FontPicture[FontPictureWidth * FontPictureHeight];

constexpr uint32_t Magenta = 0xFF00FF;
const uint32_t Palette[PaletteSize] = {0x203040, 0xC08020, 0xF0F0F0, 0x30A050};
const char* const Rows[TileSize * TileCount] = {
    "..aaaa..", ".abbbba.", "abbccbba", "abcddcba", "abcddcba", "abbccbba", ".abbbba.", "..aaaa..",
    "cccccccc", "c......c", "c.dddd.c", "c.d..d.c", "c.d..d.c", "c.dddd.c", "c......c", "cccccccc",
};

void Plot(int32_t x, int32_t y, uint32_t color)
{
    if (x >= 0 && x < Width && y >= 0 && y < Height)
    {
        Picture[y * Width + x] = color;
    }
}

void Box(int32_t left, int32_t top, int32_t width, int32_t height, uint32_t color)
{
    for (int32_t y = top; y < top + height; ++y)
    {
        for (int32_t x = left; x < left + width; ++x)
        {
            Plot(x, y, color);
        }
    }
}

uint32_t Hash(const uint32_t* pixels, size_t count)
{
    uint32_t hash = 2166136261U;
    for (size_t index = 0; index < count; ++index)
    {
        for (int shift = 0; shift < 32; shift += 8)
        {
            hash = (hash ^ ((pixels[index] >> shift) & 0xFFU)) * 16777619U;
        }
    }
    return hash;
}

uint32_t Dim(uint32_t color)
{
    const uint32_t red = (color >> 16 & 0xFFU) * 3 / 8;
    const uint32_t green = (color >> 8 & 0xFFU) * 3 / 8;
    const uint32_t blue = (color & 0xFFU) * 3 / 8 + 0x10;
    return red << 16 | green << 8 | blue;
}

void PlaceTile(int32_t tile, int32_t left, int32_t top, bool dim, bool keyed)
{
    for (int32_t row = 0; row < TileSize; ++row)
    {
        for (int32_t column = 0; column < TileSize; ++column)
        {
            const int32_t x = left + column;
            const int32_t y = top + row;
            if (x < 0 || x >= TilePictureWidth || y < 0 || y >= TilePictureHeight)
            {
                continue;
            }
            const char symbol = Rows[tile * TileSize + row][column];
            uint32_t color = Magenta;
            if (symbol != '.')
            {
                color = dim ? Dim(Palette[symbol - 'a']) : Palette[symbol - 'a'];
            }
            if (!keyed || color != Magenta)
            {
                TilePicture[y * TilePictureWidth + x] = color;
            }
        }
    }
}

struct Target
{
    uint32_t* pixels;
    int32_t width;
    int32_t height;

    void Plot(int32_t x, int32_t y, uint32_t color) const
    {
        if (x >= 0 && x < width && y >= 0 && y < height)
        {
            pixels[y * width + x] = color;
        }
    }
};

struct PlainFont
{
    const uint8_t* bytes;
    size_t size;

    uint32_t Le(size_t offset, int count) const
    {
        uint32_t value = 0;
        for (int index = count - 1; index >= 0; --index)
        {
            value = value << 8 | bytes[offset + static_cast<size_t>(index)];
        }
        return value;
    }

    int32_t Signed16(size_t offset) const
    {
        const auto value = static_cast<int32_t>(Le(offset, 2));
        return value >= 0x8000 ? value - 0x10000 : value;
    }

    // The offset of the record for a code point, by a linear scan; record 0 if there is none.
    size_t Record(uint32_t code_point) const
    {
        const size_t table = Le(32, 4);
        const uint32_t count = Le(28, 4);
        for (size_t index = 1; index < count; ++index)
        {
            if (Le(table + index * 20, 4) == code_point)
            {
                return table + index * 20;
            }
        }
        return table;
    }
};

uint32_t PlainBlend(uint32_t background, uint32_t foreground, uint32_t coverage)
{
    const uint32_t alpha = coverage * 17;
    uint32_t result = 0;
    for (int shift = 0; shift < 24; shift += 8)
    {
        const uint32_t b = background >> shift & 0xFFU;
        const uint32_t f = foreground >> shift & 0xFFU;
        result |= ((f * alpha + b * (255 - alpha) + 127) / 255) << shift;
    }
    return result;
}

// Decodes one code point: valid forms of one to four bytes, and U+FFFD for anything else, one
// per bad byte. Enough for the strings gfxcheck draws, which hold no overlong forms.
uint32_t PlainDecode(const char*& text)
{
    const auto byte = [&](int offset) { return static_cast<uint8_t>(text[offset]); };
    const uint8_t lead = byte(0);
    int length = 1;
    uint32_t code = lead;
    if (lead >= 0xF0 && lead <= 0xF4)
    {
        length = 4;
        code = lead & 0x07U;
    }
    else if (lead >= 0xE0 && lead < 0xF0)
    {
        length = 3;
        code = lead & 0x0FU;
    }
    else if (lead >= 0xC2 && lead < 0xE0)
    {
        length = 2;
        code = lead & 0x1FU;
    }
    else if (lead >= 0x80)
    {
        ++text;
        return 0xFFFD;
    }
    for (int index = 1; index < length; ++index)
    {
        if ((byte(index) & 0xC0) != 0x80)
        {
            ++text;
            return 0xFFFD;
        }
        code = code << 6 | (byte(index) & 0x3FU);
    }
    text += length;
    return code;
}

int32_t PlainText(const Target& target, const PlainFont& font, int32_t left, int32_t top,
                  const char* text, uint32_t color, bool on, uint32_t background)
{
    const int32_t ascent = font.Signed16(20);
    const int32_t line = static_cast<int32_t>(font.Le(18, 2));
    const size_t bitmaps = font.Le(40, 4);
    if (on)
    {
        int32_t width = 0;
        for (const char* scan = text; *scan != '\0';)
        {
            const uint32_t code_point = PlainDecode(scan);
            if (code_point >= 0x20 && code_point != 0x7F)
            {
                width += static_cast<int32_t>(font.Le(font.Record(code_point) + 16, 2));
            }
        }
        for (int32_t y = top; y < top + line; ++y)
        {
            for (int32_t x = left; x < left + width; ++x)
            {
                target.Plot(x, y, background);
            }
        }
    }
    for (const char* scan = text; *scan != '\0';)
    {
        const uint32_t code_point = PlainDecode(scan);
        if (code_point < 0x20 || code_point == 0x7F)
        {
            continue;
        }
        const size_t record = font.Record(code_point);
        const int32_t width = static_cast<int32_t>(font.Le(record + 8, 2));
        const int32_t height = static_cast<int32_t>(font.Le(record + 10, 2));
        const int32_t glyph_left = font.Signed16(record + 12);
        const int32_t glyph_top = font.Signed16(record + 14);
        const size_t stride = static_cast<size_t>((width + 1) / 2);
        const size_t rows = bitmaps + font.Le(record + 4, 4);
        for (int32_t row = 0; row < height; ++row)
        {
            for (int32_t column = 0; column < width; ++column)
            {
                const uint8_t pair = font.bytes[rows + static_cast<size_t>(row) * stride +
                                                static_cast<size_t>(column / 2)];
                const uint32_t coverage = (column % 2 == 0 ? pair >> 4 : pair) & 0x0FU;
                const int32_t x = left + glyph_left + column;
                const int32_t y = top + ascent - glyph_top + row;
                if (coverage != 0 && x >= 0 && x < target.width && y >= 0 && y < target.height)
                {
                    uint32_t& pixel = target.pixels[y * target.width + x];
                    pixel = PlainBlend(pixel, color, coverage);
                }
            }
        }
        left += static_cast<int32_t>(font.Le(record + 16, 2));
    }
    return left;
}

uint8_t* ReadFont(const char* path, size_t* size)
{
    FILE* file = fopen(path, "rb");
    if (file == nullptr)
    {
        return nullptr;
    }
    constexpr size_t Capacity = size_t{1} << 20;
    auto* bytes = static_cast<uint8_t*>(malloc(Capacity));
    *size = bytes == nullptr ? 0 : fread(bytes, 1, Capacity, file);
    fclose(file);
    if (*size == 0)
    {
        free(bytes);
        return nullptr;
    }
    return bytes;
}

} // namespace

const char* FontText()
{
    return "H\xC3\xA9llo \xCE\xA9\xCE\xBC\xD0\x96 \xE2\x94\x8C\xE2\x94\x80 "
           "\xE2\x98\xBA\x01\xFF! \xE2\x80\xA6 Zq";
}

const char* OnText()
{
    return "On";
}

const char* ConsoleText()
{
    return "Con\xC3\xB6le \xE2\x86\x92";
}

uint32_t ExpectedFontChecksum(const char* path)
{
    size_t size = 0;
    uint8_t* bytes = ReadFont(path, &size);
    if (bytes == nullptr)
    {
        return 0;
    }
    for (uint32_t& pixel : FontPicture)
    {
        pixel = 0x203050;
    }
    const Target target{FontPicture, FontPictureWidth, FontPictureHeight};
    PlainText(target, {bytes, size}, 4, 4, FontText(), 0xF0E0C0, false, 0);
    PlainText(target, {bytes, size}, 4, 26, OnText(), 0x00FF00, true, 0x602020);
    const PlainFont console{ConsoleFontData, ConsoleFontSize};
    PlainText(target, console, 50, 26, ConsoleText(), 0xFFFF00, false, 0);
    PlainText(target, console, 70, 40, "cut", 0xFF8080, false, 0);
    free(bytes);
    return Hash(FontPicture, FontPictureWidth * FontPictureHeight);
}

const char* const* TileRows()
{
    return Rows;
}

const uint32_t* TilePalette()
{
    return Palette;
}

uint32_t ExpectedTileChecksum()
{
    for (uint32_t& pixel : TilePicture)
    {
        pixel = 0x102030;
    }
    PlaceTile(0, 2, 2, false, true);
    PlaceTile(0, 12, 2, true, true);
    PlaceTile(1, 22, 2, false, true);
    PlaceTile(1, 32, 2, true, true);
    PlaceTile(1, -4, 12, false, false);
    PlaceTile(0, 36, 16, true, true);
    return Hash(TilePicture, TilePictureWidth * TilePictureHeight);
}

uint32_t ExpectedChecksum(const char* sans_path, const char* mono_path)
{
    size_t sans_size = 0;
    size_t mono_size = 0;
    uint8_t* sans = ReadFont(sans_path, &sans_size);
    uint8_t* mono = ReadFont(mono_path, &mono_size);
    if (sans == nullptr || mono == nullptr)
    {
        free(sans);
        free(mono);
        return 0;
    }
    Box(0, 0, Width, Height, 0x101820);
    Box(4, 4, 30, 20, 0xC03030);
    for (int32_t x = -2; x < 18; ++x)
    {
        Plot(x, -2, 0xFFFFFF);
        Plot(x, 9, 0xFFFFFF);
    }
    for (int32_t y = -2; y < 10; ++y)
    {
        Plot(-2, y, 0xFFFFFF);
        Plot(17, y, 0xFFFFFF);
    }
    const Target target{Picture, Width, Height};
    PlainText(target, {sans, sans_size}, 6, 6, "Feather", 0xFFFF00, false, 0);
    PlainText(target, {mono, mono_size}, OsX, OsY, "OS", 0x00FF00, false, 0);
    // An 8x8 sprite of magenta with a cyan 4x4 square, where magenta is the transparent color.
    Box(SpriteX + 2, SpriteY + 2, 4, 4, 0x00FFFF);
    free(sans);
    free(mono);

    return Hash(Picture, Width * Height);
}

} // namespace FeatherOS::User::GfxCheck
