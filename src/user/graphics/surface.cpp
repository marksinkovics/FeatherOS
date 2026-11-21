#include "surface.h"

#include <stdlib.h>
#include <string.h>

extern "C"
{
#include "kernel/framebuffer/font.h"
}

namespace FeatherOS::Graphics
{

namespace
{

struct Area
{
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;

    bool Empty() const
    {
        return left >= right || top >= bottom;
    }
};

int32_t Clamp(int64_t value, int32_t limit)
{
    if (value < 0)
    {
        return 0;
    }
    return value > limit ? limit : static_cast<int32_t>(value);
}

Area Clip(int32_t x, int32_t y, int32_t width, int32_t height, int32_t limit_width,
          int32_t limit_height)
{
    if (width <= 0 || height <= 0)
    {
        return {0, 0, 0, 0};
    }
    return {Clamp(x, limit_width), Clamp(y, limit_height),
            Clamp(static_cast<int64_t>(x) + width, limit_width),
            Clamp(static_cast<int64_t>(y) + height, limit_height)};
}

uint32_t* Allocate(int32_t width, int32_t height)
{
    return static_cast<uint32_t*>(
        calloc(static_cast<size_t>(width) * static_cast<size_t>(height), sizeof(uint32_t)));
}

bool ValidSize(int32_t width, int32_t height)
{
    return width > 0 && height > 0 && width <= MaximumDimension && height <= MaximumDimension;
}

} // namespace

Surface::Surface(int32_t width, int32_t height) : m_pixels(nullptr), m_width(0), m_height(0)
{
    if (!ValidSize(width, height))
    {
        return;
    }
    m_pixels = Allocate(width, height);
    if (m_pixels != nullptr)
    {
        m_width = width;
        m_height = height;
    }
}

Surface::Surface(const Surface& other) : m_pixels(nullptr), m_width(0), m_height(0)
{
    if (!other.Valid())
    {
        return;
    }
    m_pixels = Allocate(other.m_width, other.m_height);
    if (m_pixels != nullptr)
    {
        m_width = other.m_width;
        m_height = other.m_height;
        memcpy(m_pixels, other.m_pixels,
               static_cast<size_t>(m_width) * static_cast<size_t>(m_height) * sizeof(uint32_t));
    }
}

Surface::Surface(Surface&& other) noexcept
    : m_pixels(other.m_pixels), m_width(other.m_width), m_height(other.m_height)
{
    other.m_pixels = nullptr;
    other.m_width = 0;
    other.m_height = 0;
}

Surface& Surface::operator=(const Surface& other)
{
    if (this != &other)
    {
        *this = Surface(other);
    }
    return *this;
}

Surface& Surface::operator=(Surface&& other) noexcept
{
    if (this != &other)
    {
        free(m_pixels);
        m_pixels = other.m_pixels;
        m_width = other.m_width;
        m_height = other.m_height;
        other.m_pixels = nullptr;
        other.m_width = 0;
        other.m_height = 0;
    }
    return *this;
}

Surface::~Surface()
{
    free(m_pixels);
}

bool Surface::Valid() const
{
    return m_pixels != nullptr;
}

int32_t Surface::Width() const
{
    return m_width;
}

int32_t Surface::Height() const
{
    return m_height;
}

uint32_t* Surface::Pixels()
{
    return m_pixels;
}

const uint32_t* Surface::Pixels() const
{
    return m_pixels;
}

uint32_t Surface::PixelAt(int32_t x, int32_t y) const
{
    if (m_pixels == nullptr || x < 0 || y < 0 || x >= m_width || y >= m_height)
    {
        return 0;
    }
    return m_pixels[static_cast<size_t>(y) * static_cast<size_t>(m_width) + static_cast<size_t>(x)];
}

uint32_t Surface::Checksum() const
{
    // FNV-1a over the four bytes of each pixel, least significant first.
    uint32_t hash = 2166136261U;
    const size_t count = static_cast<size_t>(m_width) * static_cast<size_t>(m_height);
    for (size_t index = 0; index < count; ++index)
    {
        const uint32_t pixel = m_pixels[index] & ColorMask;
        for (int shift = 0; shift < 32; shift += 8)
        {
            hash = (hash ^ ((pixel >> shift) & 0xFFU)) * 16777619U;
        }
    }
    return hash;
}

void Surface::Fill(uint32_t color)
{
    FillRectangle(0, 0, m_width, m_height, color);
}

void Surface::FillRectangle(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t color)
{
    const Area area = Clip(x, y, width, height, m_width, m_height);
    if (m_pixels == nullptr || area.Empty())
    {
        return;
    }
    const uint32_t pixel = color & ColorMask;
    for (int32_t row = area.top; row < area.bottom; ++row)
    {
        uint32_t* destination = m_pixels + static_cast<size_t>(row) * static_cast<size_t>(m_width);
        for (int32_t column = area.left; column < area.right; ++column)
        {
            destination[column] = pixel;
        }
    }
}

void Surface::DrawRectangle(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t color)
{
    if (width <= 0 || height <= 0)
    {
        return;
    }
    FillRectangle(x, y, width, 1, color);
    if (height > 1)
    {
        FillRectangle(x, y + height - 1, width, 1, color);
    }
    FillRectangle(x, y + 1, 1, height - 2, color);
    if (width > 1)
    {
        FillRectangle(x + width - 1, y + 1, 1, height - 2, color);
    }
}

void Surface::Blit(const Surface* source, int32_t x, int32_t y)
{
    Blit(source, x, y, false, 0);
}

void Surface::BlitKeyed(const Surface* source, int32_t x, int32_t y, uint32_t transparent)
{
    Blit(source, x, y, true, transparent & ColorMask);
}

void Surface::Blit(const Surface* source, int32_t x, int32_t y, bool keyed, uint32_t transparent)
{
    if (m_pixels == nullptr || source == nullptr || source == this || !source->Valid())
    {
        return;
    }
    const Area area = Clip(x, y, source->m_width, source->m_height, m_width, m_height);
    if (area.Empty())
    {
        return;
    }
    for (int32_t row = area.top; row < area.bottom; ++row)
    {
        const uint32_t* from = source->m_pixels +
                               static_cast<size_t>(row - y) * static_cast<size_t>(source->m_width) +
                               static_cast<size_t>(area.left - x);
        uint32_t* to =
            m_pixels + static_cast<size_t>(row) * static_cast<size_t>(m_width) + area.left;
        const auto count = static_cast<size_t>(area.right - area.left);
        if (!keyed)
        {
            memcpy(to, from, count * sizeof(uint32_t));
            continue;
        }
        for (size_t column = 0; column < count; ++column)
        {
            if ((from[column] & ColorMask) != transparent)
            {
                to[column] = from[column];
            }
        }
    }
}

void Surface::DrawText(int32_t x, int32_t y, const char* text, uint32_t color, int32_t scale)
{
    DrawString(x, y, text, color, false, 0, scale);
}

void Surface::DrawTextOn(int32_t x, int32_t y, const char* text, uint32_t color,
                         uint32_t background, int32_t scale)
{
    DrawString(x, y, text, color, true, background, scale);
}

int32_t Surface::TextWidth(const char* text, int32_t scale)
{
    if (text == nullptr || scale < 1 || scale > MaximumScale)
    {
        return 0;
    }
    const size_t length = strlen(text);
    const size_t width = length * static_cast<size_t>(GlyphWidth * scale);
    return width > static_cast<size_t>(INT32_MAX) ? INT32_MAX : static_cast<int32_t>(width);
}

void Surface::DrawString(int32_t x, int32_t y, const char* text, uint32_t color, bool opaque,
                         uint32_t background, int32_t scale)
{
    if (text == nullptr || scale < 1 || scale > MaximumScale)
    {
        return;
    }
    const int32_t advance = GlyphWidth * scale;
    const int32_t height = GlyphHeight * scale;
    if (y >= m_height || static_cast<int64_t>(y) + height <= 0)
    {
        return;
    }
    int64_t position = x;
    for (; *text != '\0' && position < m_width; ++text)
    {
        if (position + advance > 0)
        {
            DrawGlyph(static_cast<int32_t>(position), y, *text, color, opaque, background, scale);
        }
        position += advance;
    }
}

void Surface::DrawGlyph(int32_t x, int32_t y, char character, uint32_t color, bool opaque,
                        uint32_t background, int32_t scale)
{
    if (opaque)
    {
        FillRectangle(x, y, GlyphWidth * scale, GlyphHeight * scale, background);
    }
    uint8_t index = 0;
    if (indexForCharacter(character, &index) != 0)
    {
        return;
    }
    const uint8_t* glyph = font16_8 + static_cast<size_t>(index) * FONT_HEIGHT;
    for (int32_t row = 0; row < GlyphHeight; ++row)
    {
        for (int32_t column = 0; column < GlyphWidth; ++column)
        {
            if ((glyph[row] & (1U << column)) != 0)
            {
                FillRectangle(x + column * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

} // namespace FeatherOS::Graphics
