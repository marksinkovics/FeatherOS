#include <limine.h>
#include <stddef.h>

#include "kernel/framebuffer/framebuffer.h"
#include "user/syscall_abi.h"

namespace
{

// The back buffer is a byte array; these types let it be accessed in wider units.
typedef uint32_t __attribute__((may_alias)) AliasedUint32;
typedef uint64_t __attribute__((may_alias)) AliasedUint64;

void WritePixel(uint8_t* destination, uint32_t bytes_per_pixel, uint32_t pixel)
{
    if (bytes_per_pixel == 4)
    {
        *reinterpret_cast<AliasedUint32*>(destination) = pixel;
        return;
    }
    for (uint32_t byte = 0; byte < bytes_per_pixel; ++byte)
    {
        destination[byte] = (uint8_t)(pixel >> (byte * 8));
    }
}

bool IsXrgb8888(const PixelFormat& format)
{
    return format.bytes_per_pixel == 4 && format.red_size == 8 && format.red_shift == 16 &&
           format.green_size == 8 && format.green_shift == 8 && format.blue_size == 8 &&
           format.blue_shift == 0;
}

} // namespace

void CopyPixelBytes(uint8_t* destination, const uint8_t* source, size_t length)
{
    size_t index = 0;
    const uintptr_t both =
        reinterpret_cast<uintptr_t>(destination) | reinterpret_cast<uintptr_t>(source);
    if ((both & 3) == 0)
    {
        // Both addresses sit 4 bytes off the same 8-byte boundary: one word aligns them.
        const bool offset_alike =
            ((reinterpret_cast<uintptr_t>(destination) ^ reinterpret_cast<uintptr_t>(source)) &
             7) == 0;
        if (offset_alike && (both & 7) != 0 && length >= 4)
        {
            *reinterpret_cast<AliasedUint32*>(destination) =
                *reinterpret_cast<const AliasedUint32*>(source);
            index = 4;
        }
        if (offset_alike)
        {
            auto* destination_words = reinterpret_cast<AliasedUint64*>(destination + index);
            const auto* source_words = reinterpret_cast<const AliasedUint64*>(source + index);
            const size_t words = (length - index) / 8;
            for (size_t word = 0; word < words; ++word)
            {
                destination_words[word] = source_words[word];
            }
            index += words * 8;
        }
        auto* destination_halves = reinterpret_cast<AliasedUint32*>(destination + index);
        const auto* source_halves = reinterpret_cast<const AliasedUint32*>(source + index);
        const size_t halves = (length - index) / 4;
        for (size_t half = 0; half < halves; ++half)
        {
            destination_halves[half] = source_halves[half];
        }
        index += halves * 4;
    }
    for (; index < length; ++index)
    {
        destination[index] = source[index];
    }
}

uint32_t PackColor(const PixelFormat& format, uint32_t color)
{
    uint32_t red = (color >> 16) & 0xffU;
    uint32_t green = (color >> 8) & 0xffU;
    uint32_t blue = color & 0xffU;
    red = (red >> (8U - format.red_size)) << format.red_shift;
    green = (green >> (8U - format.green_size)) << format.green_shift;
    blue = (blue >> (8U - format.blue_size)) << format.blue_shift;
    return red | green | blue;
}

void PackRow(const PixelFormat& format, const uint32_t* source, uint8_t* destination,
             uint32_t count)
{
    if (IsXrgb8888(format))
    {
        CopyPixelBytes(destination, reinterpret_cast<const uint8_t*>(source),
                       static_cast<size_t>(count) * 4);
        return;
    }
    for (uint32_t index = 0; index < count; ++index)
    {
        WritePixel(destination + static_cast<size_t>(index) * format.bytes_per_pixel,
                   format.bytes_per_pixel, PackColor(format, source[index]));
    }
}

size_t Framebuffer::RequiredBytes(const limine_framebuffer* framebuffer)
{
    if (framebuffer == nullptr || framebuffer->address == nullptr || framebuffer->width == 0 ||
        framebuffer->height == 0 ||
        framebuffer->width > FeatherOS::User::MaximumFramebufferDimension ||
        framebuffer->height > FeatherOS::User::MaximumFramebufferDimension ||
        framebuffer->pitch > UINT32_MAX || framebuffer->memory_model != LIMINE_FRAMEBUFFER_RGB ||
        (framebuffer->bpp != 16 && framebuffer->bpp != 24 && framebuffer->bpp != 32))
    {
        return 0;
    }

    const uint64_t bytes_per_pixel = (framebuffer->bpp + 7U) / 8U;
    const uint64_t bytes = framebuffer->pitch * framebuffer->height;
    if (framebuffer->pitch < framebuffer->width * bytes_per_pixel || bytes > UINT32_MAX ||
        framebuffer->red_mask_size == 0 || framebuffer->red_mask_size > 8 ||
        framebuffer->green_mask_size == 0 || framebuffer->green_mask_size > 8 ||
        framebuffer->blue_mask_size == 0 || framebuffer->blue_mask_size > 8 ||
        framebuffer->red_mask_shift + framebuffer->red_mask_size > framebuffer->bpp ||
        framebuffer->green_mask_shift + framebuffer->green_mask_size > framebuffer->bpp ||
        framebuffer->blue_mask_shift + framebuffer->blue_mask_size > framebuffer->bpp)
    {
        return 0;
    }
    return static_cast<size_t>(bytes);
}

bool Framebuffer::init(limine_framebuffer* framebuffer, uint8_t* back_buffer, size_t capacity)
{
    const size_t required = RequiredBytes(framebuffer);
    if (required == 0 || back_buffer == nullptr || required > capacity)
    {
        return false;
    }
    const uint32_t bytes_per_pixel = (framebuffer->bpp + 7) / 8;

    m_addr = (uint8_t*)framebuffer->address;
    m_width = (uint32_t)framebuffer->width;
    m_height = (uint32_t)framebuffer->height;
    m_pitch = (uint32_t)framebuffer->pitch;
    m_format = {bytes_per_pixel,
                framebuffer->red_mask_size,
                framebuffer->red_mask_shift,
                framebuffer->green_mask_size,
                framebuffer->green_mask_shift,
                framebuffer->blue_mask_size,
                framebuffer->blue_mask_shift};
    m_backbuffer = back_buffer;
    m_backbuffer_capacity = capacity;
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
        static_cast<size_t>(m_pitch) * y + static_cast<size_t>(m_format.bytes_per_pixel) * x;
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
    CopyPixelBytes(m_addr + offset, m_backbuffer + offset, static_cast<size_t>(m_pitch) * height);
}

void Framebuffer::draw(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    if (x >= m_width || y >= m_height)
    {
        return;
    }
    if (width > m_width - x)
    {
        width = m_width - x;
    }
    if (height > m_height - y)
    {
        height = m_height - y;
    }
    const size_t bytes = static_cast<size_t>(m_format.bytes_per_pixel) * width;
    size_t offset =
        static_cast<size_t>(m_pitch) * y + static_cast<size_t>(m_format.bytes_per_pixel) * x;
    for (uint32_t row = 0; row < height; ++row, offset += m_pitch)
    {
        CopyPixelBytes(m_addr + offset, m_backbuffer + offset, bytes);
    }
}

uint32_t Framebuffer::packColor(uint32_t color) const
{
    return PackColor(m_format, color);
}

void Framebuffer::writePixel(uint8_t* destination, uint32_t pixel) const
{
    WritePixel(destination, m_format.bytes_per_pixel, pixel);
}

void Framebuffer::fillSpan(uint8_t* destination, uint32_t count, uint32_t pixel) const
{
    if (m_format.bytes_per_pixel == 4)
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
        writePixel(destination + static_cast<size_t>(index) * m_format.bytes_per_pixel, pixel);
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
                     static_cast<size_t>(m_format.bytes_per_pixel) * x,
                 w, pixel);
    }
}

void Framebuffer::drawGlyph(const FeatherOS::Text::FontView& view,
                            const FeatherOS::Text::Glyph& glyph, uint32_t x, uint32_t y,
                            const uint32_t* palette)
{
    if (x >= m_width || y >= m_height)
    {
        return;
    }
    const int32_t cell_width = view.CellWidth();
    const int32_t cell_height = view.LineHeight();
    const int32_t left = glyph.left;
    const int32_t top = view.Ascent() - glyph.top;
    const int32_t screen_columns = static_cast<int32_t>(m_width - x);
    const int32_t screen_rows = static_cast<int32_t>(m_height - y);
    const int32_t width_limit = cell_width < screen_columns ? cell_width : screen_columns;
    const int32_t height_limit = cell_height < screen_rows ? cell_height : screen_rows;
    const int32_t first_column = left < 0 ? -left : 0;
    const int32_t first_row = top < 0 ? -top : 0;
    const int32_t end_column = glyph.width < width_limit - left ? glyph.width : width_limit - left;
    const int32_t end_row = glyph.height < height_limit - top ? glyph.height : height_limit - top;
    if (first_column >= end_column || first_row >= end_row)
    {
        return;
    }
    const size_t stride = FeatherOS::Text::FontView::BitmapStride(glyph.width);
    const uint8_t* source = view.Bitmap(glyph) + stride * static_cast<size_t>(first_row);
    const size_t bytes_per_pixel = m_format.bytes_per_pixel;
    for (int32_t row = first_row; row < end_row; ++row, source += stride)
    {
        uint8_t* destination =
            m_backbuffer + static_cast<size_t>(m_pitch) * (y + static_cast<uint32_t>(top + row)) +
            bytes_per_pixel * (x + static_cast<uint32_t>(left + first_column));
        for (int32_t column = first_column; column < end_column;
             ++column, destination += bytes_per_pixel)
        {
            const uint8_t packed = source[column >> 1];
            const uint32_t coverage = (column & 1) != 0 ? packed & 0x0FU : packed >> 4;
            if (coverage != 0)
            {
                writePixel(destination, palette[coverage]);
            }
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
        CopyPixelBytes(m_backbuffer + static_cast<size_t>(m_pitch) * row,
                       m_backbuffer + static_cast<size_t>(m_pitch) * (row + distance), m_pitch);
    }
    const uint32_t pixel = packColor(color);
    for (uint32_t row = y + height - distance; row < y + height; ++row)
    {
        fillSpan(m_backbuffer + static_cast<size_t>(m_pitch) * row, m_width, pixel);
    }
}

void Framebuffer::presentRow(int64_t x, uint32_t y, const uint32_t* pixels, uint32_t count)
{
    if (y >= m_height || pixels == nullptr)
    {
        return;
    }
    const auto columns = static_cast<int64_t>(count);
    if (x <= -columns || x >= static_cast<int64_t>(m_width))
    {
        return;
    }
    const int64_t first = x < 0 ? -x : 0;
    const int64_t end = x + columns;
    const int64_t last = end > static_cast<int64_t>(m_width) ? static_cast<int64_t>(m_width) : end;
    const int64_t screen_x = x + first;
    PackRow(m_format, pixels + first,
            m_backbuffer + static_cast<size_t>(m_pitch) * y +
                static_cast<size_t>(m_format.bytes_per_pixel) * static_cast<size_t>(screen_x),
            static_cast<uint32_t>(last - screen_x));
}

bool Framebuffer::acceptsDirectRows() const
{
    return IsXrgb8888(m_format);
}

uint8_t* Framebuffer::directRow(uint32_t x, uint32_t y, uint32_t count)
{
    if (!IsXrgb8888(m_format) || y >= m_height || x > m_width || count > m_width - x)
    {
        return nullptr;
    }
    return m_backbuffer + static_cast<size_t>(m_pitch) * y + static_cast<size_t>(4) * x;
}

uint32_t Framebuffer::GetHeight() const
{
    return m_height;
}

uint32_t Framebuffer::GetWidth() const
{
    return m_width;
}
