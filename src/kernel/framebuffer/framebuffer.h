#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdint.h>

struct stivale2_struct_tag_framebuffer;

#include "kernel/config.h"

class Framebuffer
{
public:
    void init(stivale2_struct_tag_framebuffer* fb);
    void clear(uint32_t color = FeatherOS::Color::Black);
    void drawPixel(uint32_t x, uint32_t y, uint32_t color);
    void draw();
    void drawRectangle(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
private:
    uint8_t* m_addr;
    uint32_t m_width;
    uint32_t m_height;
    uint32_t m_pitch;
    uint8_t  m_backbuffer[FeatherOS::Config::BackBufferLen];
    uint32_t m_backbuffer_len;
};

#endif