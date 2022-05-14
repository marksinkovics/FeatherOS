#include <stivale2.h>

#include "kernel/framebuffer/framebuffer.h"

void Framebuffer::init(stivale2_struct_tag_framebuffer* fb)
{
    m_addr = (uint8_t*)fb->framebuffer_addr;
    m_width = fb->framebuffer_width;
    m_height = fb->framebuffer_height;
    m_pitch = fb->framebuffer_pitch;
    m_backbuffer_len = m_height * m_pitch;
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

    ((uint32_t*)(m_backbuffer + (m_pitch * y)))[x] = color;
}

void Framebuffer::draw()
{
    for(uint32_t i = 0; i < m_backbuffer_len; i++)
    {
        ((uint8_t*)(m_addr))[i] = ((uint8_t*)m_backbuffer)[i];
    }
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
