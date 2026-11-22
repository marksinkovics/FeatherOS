#ifndef FEATHEROS_USER_GRAPHICS_SURFACE_H
#define FEATHEROS_USER_GRAPHICS_SURFACE_H

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Graphics
{

class Font;

constexpr uint32_t ColorMask = 0x00FFFFFF;
constexpr int32_t MaximumDimension = 16384;

class Surface
{
public:
    Surface(int32_t width, int32_t height);
    Surface(const Surface& other);
    Surface(Surface&& other) noexcept;
    Surface& operator=(const Surface& other);
    Surface& operator=(Surface&& other) noexcept;
    ~Surface();

    bool Valid() const;
    int32_t Width() const;
    int32_t Height() const;
    uint32_t* Pixels();
    const uint32_t* Pixels() const;
    uint32_t PixelAt(int32_t x, int32_t y) const;
    uint32_t Checksum() const;

    void Fill(uint32_t color);
    void FillRectangle(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t color);
    void DrawRectangle(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t color);
    void Blit(const Surface* source, int32_t x, int32_t y);
    void BlitKeyed(const Surface* source, int32_t x, int32_t y, uint32_t transparent);
    void BlitPart(const Surface* source, int32_t sx, int32_t sy, int32_t width, int32_t height,
                  int32_t x, int32_t y, bool keyed, uint32_t transparent);
    int32_t DrawText(const Font* font, int32_t x, int32_t y, const char* utf8, uint32_t color);
    int32_t DrawTextOn(const Font* font, int32_t x, int32_t y, const char* utf8, uint32_t color,
                       uint32_t background);

private:
    uint32_t* m_pixels;
    int32_t m_width;
    int32_t m_height;
};

} // namespace FeatherOS::Graphics

#endif
