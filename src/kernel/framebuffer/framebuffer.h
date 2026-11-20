#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdint.h>

struct limine_framebuffer;

#include "kernel/config.h"

struct PixelFormat
{
    uint32_t bytes_per_pixel;
    uint8_t red_size;
    uint8_t red_shift;
    uint8_t green_size;
    uint8_t green_shift;
    uint8_t blue_size;
    uint8_t blue_shift;
};

// Converts 0x00RRGGBB, as the hardware's masks say.
uint32_t PackColor(const PixelFormat& format, uint32_t color);
// Converts count colors into destination's pixels. Exact XRGB8888 is copied as it is, so the
// high byte of each color lands in the hardware's unused byte.
void PackRow(const PixelFormat& format, const uint32_t* source, uint8_t* destination,
             uint32_t count);

class Framebuffer
{
public:
    bool init(limine_framebuffer* framebuffer);
    void clear(uint32_t color = FeatherOS::Color::Black);
    void drawPixel(uint32_t x, uint32_t y, uint32_t color);
    // Copies the whole back buffer, or only rows [y, y + height), to the visible framebuffer.
    void draw();
    void draw(uint32_t y, uint32_t height);
    void drawRectangle(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
    void drawCharacter(uint8_t ch, uint32_t x, uint32_t y, uint32_t bg_color, uint32_t fg_color);
    void drawString(const char* str, uint32_t x, uint32_t y, uint32_t bg_color, uint32_t fg_color);
    // Moves back-buffer rows [y + distance, y + height) up by distance and fills the vacated rows.
    void scrollUp(uint32_t y, uint32_t height, uint32_t distance, uint32_t color);
    // Converts count colors into back-buffer row y from column x, which may be negative; only the
    // part inside the screen is written. It does not draw.
    void presentRow(int64_t x, uint32_t y, const uint32_t* pixels, uint32_t count);

    uint32_t GetHeight() const;
    uint32_t GetWidth() const;

private:
    uint32_t packColor(uint32_t color) const;
    void writePixel(uint8_t* destination, uint32_t pixel) const;
    void fillSpan(uint8_t* destination, uint32_t count, uint32_t pixel) const;

    uint8_t* m_addr;
    uint32_t m_width;
    uint32_t m_height;
    uint32_t m_pitch;
    PixelFormat m_format;
    alignas(8) uint8_t m_backbuffer[FeatherOS::Config::BackBufferLen];
    uint32_t m_backbuffer_len;
};

#endif