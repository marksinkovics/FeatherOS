#include <limine.h>

#include "kernel/framebuffer/framebuffer.h"

extern "C" {
    #include "kernel/framebuffer/font.h"
}

bool Framebuffer::init(limine_framebuffer *framebuffer)
{
    if (framebuffer == nullptr || framebuffer->address == nullptr ||
        framebuffer->width == 0 || framebuffer->height == 0 ||
        framebuffer->width > FeatherOS::Config::Width ||
        framebuffer->height > FeatherOS::Config::Height ||
        framebuffer->pitch > UINT32_MAX ||
        framebuffer->memory_model != LIMINE_FRAMEBUFFER_RGB ||
        (framebuffer->bpp != 16 && framebuffer->bpp != 24 && framebuffer->bpp != 32))
    {
        return false;
    }

    uint32_t bytes_per_pixel = (framebuffer->bpp + 7) / 8;
    uint64_t row_bytes = framebuffer->width * bytes_per_pixel;
    if (framebuffer->pitch < row_bytes || framebuffer->height > sizeof(m_backbuffer) / framebuffer->pitch ||
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
    for(uint32_t x = 0; x < m_width; x++)
    {
        for(uint32_t y = 0; y < m_height; y++)
        {
            drawPixel(x, y, color);
        }
    }
}

void Framebuffer::drawPixel(uint32_t x, uint32_t y, uint32_t color)
{
    if (x >= m_width)
        return;

    if (y >= m_height)
        return;

    uint32_t pixel = packColor(color);
    uint8_t* destination = m_backbuffer + (m_pitch * y) + (m_bytes_per_pixel * x);
    for (uint32_t byte = 0; byte < m_bytes_per_pixel; ++byte)
    {
        destination[byte] = (uint8_t)(pixel >> (byte * 8));
    }
}

void Framebuffer::draw()
{
    for (uint32_t row = 0; row < m_height; ++row)
    {
        uint8_t* source = m_backbuffer + (m_pitch * row);
        uint8_t* destination = m_addr + (m_pitch * row);
        for (uint32_t byte = 0; byte < m_pitch; ++byte)
        {
            destination[byte] = source[byte];
        }
    }
}

uint32_t Framebuffer::packColor(uint32_t color) const
{
    uint32_t red = (color >> 16) & 0xff;
    uint32_t green = (color >> 8) & 0xff;
    uint32_t blue = color & 0xff;
    red = (red >> (8 - m_red_mask_size)) << m_red_mask_shift;
    green = (green >> (8 - m_green_mask_size)) << m_green_mask_shift;
    blue = (blue >> (8 - m_blue_mask_size)) << m_blue_mask_shift;
    return red | green | blue;
}

void Framebuffer::drawRectangle(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    for (uint32_t i = x; i < x + w; i++)
    {
        for (uint32_t j = y; j < y + h; j++)
        {
            drawPixel(i, j, color);
        }
    }
}

void Framebuffer::drawCharacter(uint8_t ch_index, uint32_t x, uint32_t y, uint32_t bg_color, uint32_t fg_color)
{
    uint32_t offset = ((uint32_t)ch_index) * 16;
    for (uint32_t i = 0; i < 16; i++)
    {
        for (uint32_t j = 0; j < 8; j++)
        {
            if (font16_8[offset + i] & (1 << j))
            {
                drawPixel(x + j, y + i, fg_color);
            }
            else
            {
                drawPixel(x + j, y + i, bg_color);
            }
        }
    }
}

void Framebuffer::drawString(const char* str, uint32_t x, uint32_t y, uint32_t bg_color, uint32_t fg_color)
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
