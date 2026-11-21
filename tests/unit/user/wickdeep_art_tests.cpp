#include <catch2/catch_test_macros.hpp>

#include "user/graphics/tile_sheet.h"
#include "user/wickdeep/art.h"

#include <cstdint>
#include <vector>

namespace Art = FeatherOS::User::WickdeepArt;
using FeatherOS::Graphics::Shade;
using FeatherOS::Graphics::TileSheet;
using FeatherOS::Graphics::TileStatus;

namespace
{

constexpr uint32_t Key = TileSheet::TransparentColor;
constexpr uint32_t Glow = 0xFFF08C;

std::vector<uint32_t> Pixels(const TileSheet& sheet, int32_t tile, Shade shade)
{
    std::vector<uint32_t> pixels;
    for (int32_t y = 0; y < Art::TileSize; ++y)
    {
        for (int32_t x = 0; x < Art::TileSize; ++x)
        {
            pixels.push_back(sheet.PixelAt(tile, x, y, shade));
        }
    }
    return pixels;
}

int32_t Count(const std::vector<uint32_t>& pixels, uint32_t color)
{
    int32_t count = 0;
    for (const uint32_t pixel : pixels)
    {
        count += pixel == color ? 1 : 0;
    }
    return count;
}

} // namespace

TEST_CASE("wickdeep's art loads into a sheet", "[wickdeep][art]")
{
    TileSheet sheet;
    REQUIRE(Art::LoadWickdeepTiles(&sheet) == TileStatus::Ok);
    CHECK(sheet.TileCount() == Art::TileCount);
    CHECK(sheet.TileSize() == Art::TileSize);
    for (int32_t index = 0; index < Art::PaletteSize; ++index)
    {
        CHECK(Art::Palette()[index] != Key);
    }
}

TEST_CASE("wickdeep's terrain tiles are opaque and the others are keyed", "[wickdeep][art]")
{
    TileSheet sheet;
    REQUIRE(Art::LoadWickdeepTiles(&sheet) == TileStatus::Ok);
    for (int32_t tile = 0; tile < Art::TileCount; ++tile)
    {
        CAPTURE(tile);
        const auto pixels = Pixels(sheet, tile, Shade::Lit);
        const int32_t clear = Count(pixels, Key);
        if (tile <= Art::TilePlinth)
        {
            CHECK(clear == 0);
        }
        else
        {
            CHECK(clear > 0);
            CHECK(clear < Art::TileSize * Art::TileSize - 24);
        }
        CHECK(Count(Pixels(sheet, tile, Shade::Remembered), Key) == clear);
    }
}

TEST_CASE("no two of wickdeep's tiles look alike", "[wickdeep][art]")
{
    TileSheet sheet;
    REQUIRE(Art::LoadWickdeepTiles(&sheet) == TileStatus::Ok);
    for (int32_t first = 0; first < Art::TileCount; ++first)
    {
        for (int32_t second = first + 1; second < Art::TileCount; ++second)
        {
            CAPTURE(first, second);
            CHECK(Pixels(sheet, first, Shade::Lit) != Pixels(sheet, second, Shade::Lit));
        }
    }
}

TEST_CASE("only the wick-keeper and the lamp icon use the glow color", "[wickdeep][art]")
{
    TileSheet sheet;
    REQUIRE(Art::LoadWickdeepTiles(&sheet) == TileStatus::Ok);
    for (int32_t tile = 0; tile < Art::TileCount; ++tile)
    {
        CAPTURE(tile);
        const int32_t glow = Count(Pixels(sheet, tile, Shade::Lit), Glow);
        if (tile == Art::TileKeeper || tile == Art::TileLamp)
        {
            CHECK(glow > 0);
        }
        else
        {
            CHECK(glow == 0);
        }
    }
}
