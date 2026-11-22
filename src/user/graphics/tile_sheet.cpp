#include "tile_sheet.h"

#include <string.h>
#include <utility>

namespace FeatherOS::Graphics
{

TileSheet::TileSheet() : m_lit(0, 0), m_remembered(0, 0), m_count(0), m_size(0) {}

TileSheet::TileSheet(TileSheet&& other) noexcept
    : m_lit(std::move(other.m_lit)), m_remembered(std::move(other.m_remembered)),
      m_count(other.m_count), m_size(other.m_size)
{
    other.m_count = 0;
    other.m_size = 0;
}

TileSheet& TileSheet::operator=(TileSheet&& other) noexcept
{
    if (this != &other)
    {
        m_lit = std::move(other.m_lit);
        m_remembered = std::move(other.m_remembered);
        m_count = other.m_count;
        m_size = other.m_size;
        other.m_count = 0;
        other.m_size = 0;
    }
    return *this;
}

TileSheet::~TileSheet() = default;

uint32_t TileSheet::Remember(uint32_t color)
{
    const uint32_t red = (color >> 16 & 0xFFU) * 3 / 8;
    const uint32_t green = (color >> 8 & 0xFFU) * 3 / 8;
    const uint32_t blue = (color & 0xFFU) * 3 / 8;
    return red << 16 | green << 8 | (blue + 0x10);
}

TileStatus TileSheet::Load(const char* const* rows, int32_t tile_count, int32_t tile_size,
                           const uint32_t* palette, int32_t palette_size)
{
    if (rows == nullptr || palette == nullptr || tile_count <= 0 || tile_size <= 0 ||
        palette_size <= 0 || palette_size > MaximumPalette ||
        static_cast<int64_t>(tile_count) * tile_size > MaximumDimension ||
        tile_size > MaximumDimension)
    {
        return TileStatus::BadArgument;
    }
    uint32_t colors[MaximumPalette];
    for (int32_t index = 0; index < palette_size; ++index)
    {
        colors[index] = palette[index] & ColorMask;
        if (colors[index] == TransparentColor)
        {
            return TileStatus::BadPalette;
        }
    }
    Surface lit(tile_count * tile_size, tile_size);
    Surface remembered(tile_count * tile_size, tile_size);
    if (!lit.Valid() || !remembered.Valid())
    {
        return TileStatus::OutOfMemory;
    }
    const auto size = static_cast<size_t>(tile_size);
    for (int32_t tile = 0; tile < tile_count; ++tile)
    {
        for (int32_t y = 0; y < tile_size; ++y)
        {
            const char* row = rows[static_cast<size_t>(tile) * size + static_cast<size_t>(y)];
            if (row == nullptr || strlen(row) != size)
            {
                return TileStatus::BadLength;
            }
            for (int32_t x = 0; x < tile_size; ++x)
            {
                const char symbol = row[x];
                uint32_t color = TransparentColor;
                uint32_t dimmed = TransparentColor;
                if (symbol != '.')
                {
                    if (symbol < 'a' || symbol > 'p')
                    {
                        return TileStatus::BadCharacter;
                    }
                    const int32_t index = symbol - 'a';
                    if (index >= palette_size)
                    {
                        return TileStatus::BadIndex;
                    }
                    color = colors[index];
                    dimmed = Remember(color);
                }
                const size_t offset = static_cast<size_t>(y) * static_cast<size_t>(lit.Width()) +
                                      static_cast<size_t>(tile) * size + static_cast<size_t>(x);
                lit.Pixels()[offset] = color;
                remembered.Pixels()[offset] = dimmed;
            }
        }
    }
    m_lit = std::move(lit);
    m_remembered = std::move(remembered);
    m_count = tile_count;
    m_size = tile_size;
    return TileStatus::Ok;
}

bool TileSheet::Loaded() const
{
    return m_count > 0;
}

int32_t TileSheet::TileCount() const
{
    return m_count;
}

int32_t TileSheet::TileSize() const
{
    return m_size;
}

uint32_t TileSheet::PixelAt(int32_t tile, int32_t x, int32_t y, Shade shade) const
{
    if (tile < 0 || tile >= m_count || x < 0 || x >= m_size)
    {
        return 0;
    }
    const Surface& atlas = shade == Shade::Remembered ? m_remembered : m_lit;
    return atlas.PixelAt(tile * m_size + x, y);
}

void TileSheet::Draw(Surface* target, int32_t tile, int32_t x, int32_t y, Shade shade,
                     bool keyed) const
{
    if (target == nullptr || tile < 0 || tile >= m_count)
    {
        return;
    }
    const Surface& atlas = shade == Shade::Remembered ? m_remembered : m_lit;
    target->BlitPart(&atlas, tile * m_size, 0, m_size, m_size, x, y, keyed, TransparentColor);
}

} // namespace FeatherOS::Graphics
