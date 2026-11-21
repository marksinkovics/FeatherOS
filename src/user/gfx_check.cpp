#include "gfx_check.h"

#include <stddef.h>

extern "C" const uint8_t font16_8[];

namespace FeatherOS::User::GfxCheck
{

namespace
{

constexpr int32_t GlyphWidth = 8;
constexpr int32_t GlyphHeight = 16;
constexpr int32_t Scale = 2;
constexpr int32_t SpriteX = 50;
constexpr int32_t SpriteY = 30;

uint32_t Picture[Width * Height];
uint32_t TilePicture[TilePictureWidth * TilePictureHeight];

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

void Text(int32_t left, int32_t top, const char* text, uint32_t color, int32_t scale)
{
    for (; *text != '\0'; ++text, left += GlyphWidth * scale)
    {
        if (*text < '!' || *text > '~')
        {
            continue;
        }
        const int32_t index = *text - '!';
        for (int32_t row = 0; row < GlyphHeight; ++row)
        {
            for (int32_t column = 0; column < GlyphWidth; ++column)
            {
                if ((font16_8[index * GlyphHeight + row] >> column & 1) != 0)
                {
                    Box(left + column * scale, top + row * scale, scale, scale, color);
                }
            }
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

} // namespace

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

uint32_t ExpectedChecksum()
{
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
    Text(6, 6, "Feather", 0xFFFF00, 1);
    Text(20, 24, "OS", 0x00FF00, Scale);
    // An 8x8 sprite of magenta with a cyan 4x4 square, where magenta is the transparent color.
    Box(SpriteX + 2, SpriteY + 2, 4, 4, 0x00FFFF);

    return Hash(Picture, Width * Height);
}

} // namespace FeatherOS::User::GfxCheck
