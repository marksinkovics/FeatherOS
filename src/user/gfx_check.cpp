#include "gfx_check.h"

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

} // namespace

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

    uint32_t hash = 2166136261U;
    for (const uint32_t pixel : Picture)
    {
        for (int shift = 0; shift < 32; shift += 8)
        {
            hash = (hash ^ ((pixel >> shift) & 0xFFU)) * 16777619U;
        }
    }
    return hash;
}

} // namespace FeatherOS::User::GfxCheck
