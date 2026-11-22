#include "surface.h"

#include "../text/font_data.h"
#include "../text/utf8.h"
#include "font.h"

#include <stdlib.h>
#include <string.h>

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
    if (source != nullptr)
    {
        BlitPart(source, 0, 0, source->m_width, source->m_height, x, y, false, 0);
    }
}

void Surface::BlitKeyed(const Surface* source, int32_t x, int32_t y, uint32_t transparent)
{
    if (source != nullptr)
    {
        BlitPart(source, 0, 0, source->m_width, source->m_height, x, y, true, transparent);
    }
}

void Surface::BlitPart(const Surface* source, int32_t sx, int32_t sy, int32_t width, int32_t height,
                       int32_t x, int32_t y, bool keyed, uint32_t transparent)
{
    if (m_pixels == nullptr || source == nullptr || source == this || !source->Valid() ||
        width <= 0 || height <= 0)
    {
        return;
    }
    // The part is cut to the source first; the position moves by what was cut off.
    const int32_t part_left = Clamp(sx, source->m_width);
    const int32_t part_top = Clamp(sy, source->m_height);
    const int32_t part_right = Clamp(static_cast<int64_t>(sx) + width, source->m_width);
    const int32_t part_bottom = Clamp(static_cast<int64_t>(sy) + height, source->m_height);
    if (part_left >= part_right || part_top >= part_bottom)
    {
        return;
    }
    const int64_t origin_x = static_cast<int64_t>(x) + (part_left - static_cast<int64_t>(sx));
    const int64_t origin_y = static_cast<int64_t>(y) + (part_top - static_cast<int64_t>(sy));
    const int32_t left = Clamp(origin_x, m_width);
    const int32_t top = Clamp(origin_y, m_height);
    const int32_t right = Clamp(origin_x + (part_right - part_left), m_width);
    const int32_t bottom = Clamp(origin_y + (part_bottom - part_top), m_height);
    if (left >= right || top >= bottom)
    {
        return;
    }
    const uint32_t key = transparent & ColorMask;
    for (int32_t row = top; row < bottom; ++row)
    {
        const int64_t source_row = part_top + (row - origin_y);
        const int64_t source_column = part_left + (left - origin_x);
        const uint32_t* from =
            source->m_pixels +
            static_cast<size_t>(source_row) * static_cast<size_t>(source->m_width) +
            static_cast<size_t>(source_column);
        uint32_t* to = m_pixels + static_cast<size_t>(row) * static_cast<size_t>(m_width) + left;
        const auto count = static_cast<size_t>(right - left);
        if (!keyed)
        {
            memcpy(to, from, count * sizeof(uint32_t));
            continue;
        }
        for (size_t column = 0; column < count; ++column)
        {
            if ((from[column] & ColorMask) != key)
            {
                to[column] = from[column];
            }
        }
    }
}

int32_t Surface::DrawText(const Font* font, int32_t x, int32_t y, const char* utf8, uint32_t color)
{
    if (font == nullptr || !font->Valid() || utf8 == nullptr)
    {
        return x;
    }
    const Text::FontView& view = font->View();
    const uint32_t foreground = color & ColorMask;
    const int64_t baseline = static_cast<int64_t>(y) + view.Ascent();
    const size_t length = strlen(utf8);
    int64_t pen = x;
    for (size_t offset = 0; offset < length;)
    {
        size_t used = 0;
        const char32_t code_point = Text::DecodeNext(utf8 + offset, length - offset, &used);
        offset += used;
        if (code_point < 0x20 || code_point == 0x7F)
        {
            continue;
        }
        const Text::Glyph glyph = view.Find(code_point);
        if (m_pixels != nullptr)
        {
            const int64_t left = pen + glyph.left;
            const int64_t top = baseline - glyph.top;
            const int32_t first_column = Clamp(-left, glyph.width);
            const int32_t last_column = Clamp(static_cast<int64_t>(m_width) - left, glyph.width);
            const int32_t first_row = Clamp(-top, glyph.height);
            const int32_t last_row = Clamp(static_cast<int64_t>(m_height) - top, glyph.height);
            for (int32_t row = first_row; row < last_row; ++row)
            {
                uint32_t* line =
                    m_pixels + static_cast<size_t>(top + row) * static_cast<size_t>(m_width);
                for (int32_t column = first_column; column < last_column; ++column)
                {
                    const uint32_t coverage = view.Coverage(glyph, column, row);
                    if (coverage != 0)
                    {
                        uint32_t& pixel = line[left + column];
                        pixel = Text::Blend(pixel & ColorMask, foreground, coverage);
                    }
                }
            }
        }
        pen += glyph.advance;
    }
    return pen > INT32_MAX ? INT32_MAX : static_cast<int32_t>(pen);
}

int32_t Surface::DrawTextOn(const Font* font, int32_t x, int32_t y, const char* utf8,
                            uint32_t color, uint32_t background)
{
    if (font == nullptr || !font->Valid() || utf8 == nullptr)
    {
        return x;
    }
    FillRectangle(x, y, font->MeasureText(utf8), font->LineHeight(), background);
    return DrawText(font, x, y, utf8, color);
}

} // namespace FeatherOS::Graphics
