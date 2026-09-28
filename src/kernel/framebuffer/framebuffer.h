#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdint.h>

struct limine_framebuffer;

#include "kernel/config.h"

class Framebuffer
{
public:
    bool init(limine_framebuffer *framebuffer);
    void clear(uint32_t color = FeatherOS::Color::Black);
    void drawPixel(uint32_t x, uint32_t y, uint32_t color);
    void draw();
    void drawRectangle(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
    void drawCharacter(uint8_t ch, uint32_t x, uint32_t y, uint32_t bg_color, uint32_t fg_color);
    void drawString(const char* str, uint32_t x, uint32_t y, uint32_t bg_color, uint32_t fg_color);

    uint32_t GetHeight() const;
    uint32_t GetWidth() const;

private:
    uint32_t packColor(uint32_t color) const;

    uint8_t* m_addr;
    uint32_t m_width;
    uint32_t m_height;
    uint32_t m_pitch;
    uint32_t m_bytes_per_pixel;
    uint8_t m_red_mask_size;
    uint8_t m_red_mask_shift;
    uint8_t m_green_mask_size;
    uint8_t m_green_mask_shift;
    uint8_t m_blue_mask_size;
    uint8_t m_blue_mask_shift;
    uint8_t  m_backbuffer[FeatherOS::Config::BackBufferLen];
    uint32_t m_backbuffer_len;
};

#endif