#ifndef FEATHEROS_USER_GRAPHICS_TILE_SHEET_H
#define FEATHEROS_USER_GRAPHICS_TILE_SHEET_H

#include "surface.h"

#include <stdint.h>

namespace FeatherOS::Graphics
{

enum class TileStatus : int32_t
{
    Ok = 0,
    BadArgument,
    BadLength,
    BadCharacter,
    BadIndex,
    BadPalette,
    OutOfMemory,
};

enum class Shade : int32_t
{
    Lit = 0,
    Remembered = 1,
};

class TileSheet
{
public:
    static constexpr int32_t MaximumPalette = 16;
    // Transparent in both atlases, so no palette color may equal it.
    static constexpr uint32_t TransparentColor = 0xFF00FF;

    TileSheet();
    TileSheet(const TileSheet&) = delete;
    TileSheet(TileSheet&& other) noexcept;
    TileSheet& operator=(const TileSheet&) = delete;
    TileSheet& operator=(TileSheet&& other) noexcept;
    ~TileSheet();

    TileStatus Load(const char* const* rows, int32_t tile_count, int32_t tile_size,
                    const uint32_t* palette, int32_t palette_size);
    bool Loaded() const;
    int32_t TileCount() const;
    int32_t TileSize() const;
    uint32_t PixelAt(int32_t tile, int32_t x, int32_t y, Shade shade) const;
    void Draw(Surface* target, int32_t tile, int32_t x, int32_t y, Shade shade, bool keyed) const;

    static uint32_t Remember(uint32_t color);

private:
    Surface m_lit;
    Surface m_remembered;
    int32_t m_count;
    int32_t m_size;
};

} // namespace FeatherOS::Graphics

#endif
