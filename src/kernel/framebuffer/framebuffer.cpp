#include <limine.h>
#include <stddef.h>

#include "kernel/framebuffer/framebuffer.h"

extern "C"
{
#include "kernel/framebuffer/font.h"
}

namespace
{

// The back buffer is a byte array; these types let it be accessed in wider units.
typedef uint32_t __attribute__((may_alias)) AliasedUint32;
typedef uint64_t __attribute__((may_alias)) AliasedUint64;

void CopyBytes(uint8_t* destination, const uint8_t* source, size_t length)
{
    size_t index = 0;
    if (((reinterpret_cast<uintptr_t>(destination) | reinterpret_cast<uintptr_t>(source)) & 7) == 0)
    {
        auto* destination_words = reinterpret_cast<AliasedUint64*>(destination);
        const auto* source_words = reinterpret_cast<const AliasedUint64*>(source);
        const size_t words = length / 8;
        for (; index < words; ++index)
        {
            destination_words[index] = source_words[index];
        }
        index *= 8;
    }
    for (; index < length; ++index)
    {
        destination[index] = source[index];
    }
}

} // namespace

bool Framebuffer::init(limine_framebuffer* framebuffer)
{
    if (framebuffer == nullptr || framebuffer->address == nullptr || framebuffer->width == 0 ||
        framebuffer->height == 0 || framebuffer->width > FeatherOS::Config::Width ||
        framebuffer->height > FeatherOS::Config::Height || framebuffer->pitch > UINT32_MAX ||
        framebuffer->memory_model != LIMINE_FRAMEBUFFER_RGB ||
        (framebuffer->bpp != 16 && framebuffer->bpp != 24 && framebuffer->bpp != 32))
    {
        return false;
    }

    uint32_t bytes_per_pixel = (framebuffer->bpp + 7) / 8;
    uint64_t row_bytes = framebuffer->width * bytes_per_pixel;
    if (framebuffer->pitch < row_bytes ||
        framebuffer->height > sizeof(m_backbuffer) / framebuffer->pitch ||
        framebuffer->red_mask_size == 0 || framebuffer->red_mask_size > 8 ||
        framebuffer->green_mask_size == 0 || framebuffer->green_mask_size > 8 ||
        framebuffer->blue_mask_size == 0 || framebuffer->blue_mask_size > 8 ||
        framebuffer->red_mask_shift + framebuffer->red_mask_size > framebuffer->bpp ||
        framebuffer->green_mask_shift + framebuffer->green_mask_size > framebuffer->bpp ||
        framebuffer->blue_mask_shift + framebuffer->blue_mask_size > framebuffer->bpp)
    {
        return false;
    }

    m_addr = (uint8_t*)framebuffer->address;
    m_width = (uint32_t)framebuffer->width;
    m_height = (uint32_t)framebuffer->height;
    m_pitch = (uint32_t)framebuffer->pitch;
    m_bytes_per_pixel = bytes_per_pixel;
    m_red_mask_size = framebuffer->red_mask_size;
    m_red_mask_shift = framebuffer->red_mask_shift;
    m_green_mask_size = framebuffer->green_mask_size;
    m_green_mask_shift = framebuffer->green_mask_shift;
    m_blue_mask_size = framebuffer->blue_mask_size;
    m_blue_mask_shift = framebuffer->blue_mask_shift;
    m_backbuffer_len = m_height * m_pitch;
    for (uint32_t byte = 0; byte < m_backbuffer_len; ++byte)
    {
        m_backbuffer[byte] = 0;
    }
    return true;
}

void Framebuffer::clear(uint32_t color)
{
    const uint32_t pixel = packColor(color);
    for (uint32_t y = 0; y < m_height; ++y)
    {
        fillSpan(m_backbuffer + static_cast<size_t>(m_pitch) * y, m_width, pixel);
    }
}

void Framebuffer::drawPixel(uint32_t x, uint32_t y, uint32_t color)
{
    if (x >= m_width)
        return;

    if (y >= m_height)
        return;

    size_t pixel_offset =
        static_cast<size_t>(m_pitch) * y + static_cast<size_t>(m_bytes_per_pixel) * x;
    writePixel(m_backbuffer + pixel_offset, packColor(color));
}

void Framebuffer::draw()
{
    draw(0, m_height);
}

void Framebuffer::draw(uint32_t y, uint32_t height)
{
    if (y >= m_height)
    {
        return;
    }
    if (height > m_height - y)
    {
        height = m_height - y;
    }
    // Rows are contiguous, so the requested band is one copy.
    const size_t offset = static_cast<size_t>(m_pitch) * y;
    CopyBytes(m_addr + offset, m_backbuffer + offset, static_cast<size_t>(m_pitch) * height);
}

uint32_t Framebuffer::packColor(uint32_t color) const
{
    uint32_t red = (color >> 16) & 0xffU;
    uint32_t green = (color >> 8) & 0xffU;
    uint32_t blue = color & 0xffU;
    red = (red >> (8U - m_red_mask_size)) << m_red_mask_shift;
    green = (green >> (8U - m_green_mask_size)) << m_green_mask_shift;
    blue = (blue >> (8U - m_blue_mask_size)) << m_blue_mask_shift;
    return red | green | blue;
}

void Framebuffer::writePixel(uint8_t* destination, uint32_t pixel) const
{
    if (m_bytes_per_pixel == 4)
    {
        *reinterpret_cast<AliasedUint32*>(destination) = pixel;
        return;
    }
    for (uint32_t byte = 0; byte < m_bytes_per_pixel; ++byte)
    {
        destination[byte] = (uint8_t)(pixel >> (byte * 8));
    }
}

void Framebuffer::fillSpan(uint8_t* destination, uint32_t count, uint32_t pixel) const
{
    if (m_bytes_per_pixel == 4)
    {
        auto* pixels = reinterpret_cast<AliasedUint32*>(destination);
        for (uint32_t index = 0; index < count; ++index)
        {
            pixels[index] = pixel;
        }
        return;
    }
    for (uint32_t index = 0; index < count; ++index)
    {
        writePixel(destination + index * m_bytes_per_pixel, pixel);
    }
}

void Framebuffer::drawRectangle(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    if (x >= m_width || y >= m_height)
    {
        return;
    }
    if (w > m_width - x)
    {
        w = m_width - x;
    }
    if (h > m_height - y)
    {
        h = m_height - y;
    }
    const uint32_t pixel = packColor(color);
    for (uint32_t row = y; row < y + h; ++row)
    {
        fillSpan(m_backbuffer + static_cast<size_t>(m_pitch) * row +
                     static_cast<size_t>(m_bytes_per_pixel) * x,
                 w, pixel);
    }
}

void Framebuffer::drawCharacter(uint8_t ch_index, uint32_t x, uint32_t y, uint32_t bg_color,
                                uint32_t fg_color)
{
    if (x >= m_width || y >= m_height)
    {
        return;
    }
    const uint32_t columns = m_width - x < FONT_WIDTH ? m_width - x : FONT_WIDTH;
    const uint32_t rows = m_height - y < FONT_HEIGHT ? m_height - y : FONT_HEIGHT;
    const uint32_t fg_pixel = packColor(fg_color);
    const uint32_t bg_pixel = packColor(bg_color);
    const uint8_t* glyph = font16_8 + static_cast<uint32_t>(ch_index) * FONT_HEIGHT;
    for (uint32_t i = 0; i < rows; i++)
    {
        uint8_t* destination = m_backbuffer + static_cast<size_t>(m_pitch) * (y + i) +
                               static_cast<size_t>(m_bytes_per_pixel) * x;
        const uint8_t bits = glyph[i];
        for (uint32_t j = 0; j < columns; j++)
        {
            writePixel(destination + j * m_bytes_per_pixel,
                       (bits & (1U << j)) != 0 ? fg_pixel : bg_pixel);
        }
    }
}

void Framebuffer::scrollUp(uint32_t y, uint32_t height, uint32_t distance, uint32_t color)
{
    if (y >= m_height)
    {
        return;
    }
    if (height > m_height - y)
    {
        height = m_height - y;
    }
    if (distance > height)
    {
        distance = height;
    }
    // Copying forward is safe because each destination row lies above its source row.
    for (uint32_t row = y; row + distance < y + height; ++row)
    {
        CopyBytes(m_backbuffer + static_cast<size_t>(m_pitch) * row,
                  m_backbuffer + static_cast<size_t>(m_pitch) * (row + distance), m_pitch);
    }
    const uint32_t pixel = packColor(color);
    for (uint32_t row = y + height - distance; row < y + height; ++row)
    {
        fillSpan(m_backbuffer + static_cast<size_t>(m_pitch) * row, m_width, pixel);
    }
}

void Framebuffer::drawString(const char* str, uint32_t x, uint32_t y, uint32_t bg_color,
                             uint32_t fg_color)
{
    if (str == nullptr)
    {
        return;
    }

    int i = 0;
    char character = str[0];
    while (character != '\0')
    {
        uint32_t character_x = x + (i * 8) + 2;
        uint32_t character_y = y;
        uint8_t font_index = 0;
        if (indexForCharacter(character, &font_index) == 0)
        {
            drawCharacter(font_index, character_x, character_y, bg_color, fg_color);
        }
        character = str[++i];
    }
}

uint32_t Framebuffer::GetHeight() const
{
    return m_height;
}

uint32_t Framebuffer::GetWidth() const
{
    return m_width;
}
