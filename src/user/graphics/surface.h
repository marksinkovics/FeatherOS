#ifndef FEATHEROS_USER_GRAPHICS_SURFACE_H
#define FEATHEROS_USER_GRAPHICS_SURFACE_H

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Graphics
{

constexpr uint32_t ColorMask = 0x00FFFFFF;
constexpr int32_t GlyphWidth = 8;
constexpr int32_t GlyphHeight = 16;
constexpr int32_t MaximumDimension = 16384;
constexpr int32_t MaximumScale = 64;

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
    void DrawText(int32_t x, int32_t y, const char* text, uint32_t color, int32_t scale);
    void DrawTextOn(int32_t x, int32_t y, const char* text, uint32_t color, uint32_t background,
                    int32_t scale);

    static int32_t TextWidth(const char* text, int32_t scale);

private:
    void DrawString(int32_t x, int32_t y, const char* text, uint32_t color, bool opaque,
                    uint32_t background, int32_t scale);
    void DrawGlyph(int32_t x, int32_t y, char character, uint32_t color, bool opaque,
                   uint32_t background, int32_t scale);

    uint32_t* m_pixels;
    int32_t m_width;
    int32_t m_height;
};

} // namespace FeatherOS::Graphics

#endif
