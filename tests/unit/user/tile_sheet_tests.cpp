#include <catch2/catch_test_macros.hpp>

#include "user/gfx_check.h"
#include "user/graphics/surface.h"
#include "user/graphics/tile_sheet.h"

#include <array>
#include <cstdint>
#include <utility>

namespace Graphics = FeatherOS::Graphics;
using Graphics::Shade;
using Graphics::Surface;
using Graphics::TileSheet;
using Graphics::TileStatus;

namespace
{

constexpr uint32_t Key = TileSheet::TransparentColor;
constexpr std::array<uint32_t, 3> Palette = {0x204060, 0xFF8000, 0x000000};

const char* const TwoTiles[] = {
    ".a",
    "bc",
    "aa",
    "..",
};

TileStatus LoadTwo(TileSheet& sheet)
{
    return sheet.Load(TwoTiles, 2, 2, Palette.data(), 3);
}

} // namespace

TEST_CASE("a sheet of text tiles loads into both atlases", "[graphics][tiles]")
{
    TileSheet sheet;
    CHECK_FALSE(sheet.Loaded());
    REQUIRE(LoadTwo(sheet) == TileStatus::Ok);
    CHECK(sheet.Loaded());
    CHECK(sheet.TileCount() == 2);
    CHECK(sheet.TileSize() == 2);

    CHECK(sheet.PixelAt(0, 0, 0, Shade::Lit) == Key);
    CHECK(sheet.PixelAt(0, 1, 0, Shade::Lit) == 0x204060);
    CHECK(sheet.PixelAt(0, 0, 1, Shade::Lit) == 0xFF8000);
    CHECK(sheet.PixelAt(0, 1, 1, Shade::Lit) == 0x000000);
    CHECK(sheet.PixelAt(1, 0, 0, Shade::Lit) == 0x204060);
    CHECK(sheet.PixelAt(1, 0, 1, Shade::Lit) == Key);
    CHECK(sheet.PixelAt(1, 1, 1, Shade::Lit) == Key);

    CHECK(sheet.PixelAt(0, 0, 0, Shade::Remembered) == Key);
    CHECK(sheet.PixelAt(1, 1, 1, Shade::Remembered) == Key);
    CHECK(sheet.PixelAt(0, 1, 1, Shade::Remembered) == 0x000010);

    CHECK(sheet.PixelAt(2, 0, 0, Shade::Lit) == 0);
    CHECK(sheet.PixelAt(-1, 0, 0, Shade::Lit) == 0);
    CHECK(sheet.PixelAt(0, 2, 0, Shade::Lit) == 0);
    CHECK(sheet.PixelAt(0, 0, 2, Shade::Lit) == 0);
}

TEST_CASE("remembered colors are three eighths of each channel plus a blue tint",
          "[graphics][tiles]")
{
    CHECK(TileSheet::Remember(0x000000) == 0x000010);
    CHECK(TileSheet::Remember(0xFFFFFF) == 0x5F5F6F);
    CHECK(TileSheet::Remember(0x204060) == 0x0C1824 + 0x10);
    CHECK(TileSheet::Remember(0xFF8000) == 0x5F3000 + 0x10);

    TileSheet sheet;
    REQUIRE(LoadTwo(sheet) == TileStatus::Ok);
    CHECK(sheet.PixelAt(0, 1, 0, Shade::Remembered) == TileSheet::Remember(0x204060));
    CHECK(sheet.PixelAt(0, 0, 1, Shade::Remembered) == TileSheet::Remember(0xFF8000));
    CHECK(sheet.PixelAt(1, 0, 0, Shade::Remembered) == 0x0C1834);
    CHECK(TileSheet::Remember(0xFF8000) != Key);
    CHECK(TileSheet::Remember(0xFFFFFF) != Key);
}

TEST_CASE("every malformed sheet is refused with its own status", "[graphics][tiles]")
{
    TileSheet sheet;

    SECTION("arguments")
    {
        CHECK(sheet.Load(nullptr, 2, 2, Palette.data(), 3) == TileStatus::BadArgument);
        CHECK(sheet.Load(TwoTiles, 2, 2, nullptr, 3) == TileStatus::BadArgument);
        CHECK(sheet.Load(TwoTiles, 0, 2, Palette.data(), 3) == TileStatus::BadArgument);
        CHECK(sheet.Load(TwoTiles, 2, 0, Palette.data(), 3) == TileStatus::BadArgument);
        CHECK(sheet.Load(TwoTiles, -1, 2, Palette.data(), 3) == TileStatus::BadArgument);
        CHECK(sheet.Load(TwoTiles, 2, 2, Palette.data(), 0) == TileStatus::BadArgument);
        CHECK(sheet.Load(TwoTiles, 2, 2, Palette.data(), 17) == TileStatus::BadArgument);
        CHECK(sheet.Load(TwoTiles, INT32_MAX, 2, Palette.data(), 3) == TileStatus::BadArgument);
        CHECK(sheet.Load(TwoTiles, 2, INT32_MAX, Palette.data(), 3) == TileStatus::BadArgument);
    }
    SECTION("row lengths")
    {
        const char* const shorter[] = {".a", "b", "aa", ".."};
        const char* const longer[] = {".a", "bc", "aa", "..."};
        const char* const empty[] = {".a", "bc", "", ".."};
        const char* const missing[] = {".a", "bc", nullptr, ".."};
        CHECK(sheet.Load(shorter, 2, 2, Palette.data(), 3) == TileStatus::BadLength);
        CHECK(sheet.Load(longer, 2, 2, Palette.data(), 3) == TileStatus::BadLength);
        CHECK(sheet.Load(empty, 2, 2, Palette.data(), 3) == TileStatus::BadLength);
        CHECK(sheet.Load(missing, 2, 2, Palette.data(), 3) == TileStatus::BadLength);
    }
    SECTION("characters")
    {
        for (const char bad : {'A', 'q', '#', ' ', '0', 'z', '\x7f', '\xe9'})
        {
            const char row[3] = {'a', bad, '\0'};
            const char* const rows[] = {".a", row, "aa", ".."};
            CHECK(sheet.Load(rows, 2, 2, Palette.data(), 3) == TileStatus::BadCharacter);
        }
    }
    SECTION("indexes past the palette")
    {
        const char* const rows[] = {".a", "bd", "aa", ".."};
        CHECK(sheet.Load(rows, 2, 2, Palette.data(), 3) == TileStatus::BadIndex);
        const std::array<uint32_t, 4> four = {0x204060, 0xFF8000, 0x000000, 0x00FF00};
        CHECK(sheet.Load(rows, 2, 2, four.data(), 4) == TileStatus::Ok);
        const char* const last[] = {"p."};
        std::array<uint32_t, 16> sixteen{};
        CHECK(sheet.Load(last, 1, 1, sixteen.data(), 16) == TileStatus::BadLength);
        const char* const single[] = {"p"};
        CHECK(sheet.Load(single, 1, 1, sixteen.data(), 16) == TileStatus::Ok);
        CHECK(sheet.Load(single, 1, 1, sixteen.data(), 15) == TileStatus::BadIndex);
    }
    SECTION("a palette holding the transparent color")
    {
        const std::array<uint32_t, 2> magenta = {0x000000, 0xFF000000 | Key};
        CHECK(sheet.Load(TwoTiles, 2, 2, magenta.data(), 2) == TileStatus::BadPalette);
    }
}

TEST_CASE("a refused load keeps the sheet that was there", "[graphics][tiles]")
{
    TileSheet sheet;
    REQUIRE(LoadTwo(sheet) == TileStatus::Ok);
    const char* const bad[] = {"ab", "cc", "a?", "bb", "cc", "dd"};
    CHECK(sheet.Load(bad, 3, 2, Palette.data(), 3) == TileStatus::BadCharacter);
    CHECK(sheet.TileCount() == 2);
    CHECK(sheet.PixelAt(0, 1, 0, Shade::Lit) == 0x204060);
    const char* const one[] = {"cc", "cc"};
    CHECK(sheet.Load(one, 1, 2, Palette.data(), 3) == TileStatus::Ok);
    CHECK(sheet.TileCount() == 1);
    CHECK(sheet.PixelAt(0, 0, 0, Shade::Lit) == 0x000000);
}

TEST_CASE("a moved sheet keeps its tiles and leaves nothing behind", "[graphics][tiles]")
{
    TileSheet sheet;
    REQUIRE(LoadTwo(sheet) == TileStatus::Ok);
    TileSheet moved(std::move(sheet));
    CHECK_FALSE(sheet.Loaded());
    CHECK(moved.TileCount() == 2);
    CHECK(moved.PixelAt(0, 1, 0, Shade::Lit) == 0x204060);
    Surface target(4, 4);
    sheet.Draw(&target, 0, 0, 0, Shade::Lit, false);
    CHECK(target.Checksum() == Surface(4, 4).Checksum());

    TileSheet assigned;
    assigned = std::move(moved);
    CHECK_FALSE(moved.Loaded());
    CHECK(assigned.TileSize() == 2);
    CHECK(assigned.PixelAt(1, 0, 0, Shade::Lit) == 0x204060);
}

TEST_CASE("tiles are drawn keyed or not and clipped", "[graphics][tiles]")
{
    TileSheet sheet;
    REQUIRE(LoadTwo(sheet) == TileStatus::Ok);
    constexpr uint32_t Back = 0x111111;
    Surface target(5, 4);
    target.Fill(Back);

    SECTION("keyed leaves the background where the tile is transparent")
    {
        sheet.Draw(&target, 0, 1, 1, Shade::Lit, true);
        CHECK(target.PixelAt(1, 1) == Back);
        CHECK(target.PixelAt(2, 1) == 0x204060);
        CHECK(target.PixelAt(1, 2) == 0xFF8000);
        CHECK(target.PixelAt(2, 2) == 0x000000);
    }
    SECTION("unkeyed copies the transparent color too")
    {
        sheet.Draw(&target, 0, 1, 1, Shade::Lit, false);
        CHECK(target.PixelAt(1, 1) == Key);
        CHECK(target.PixelAt(2, 1) == 0x204060);
    }
    SECTION("the second tile and the remembered shade")
    {
        sheet.Draw(&target, 1, 3, 0, Shade::Remembered, true);
        CHECK(target.PixelAt(3, 0) == TileSheet::Remember(0x204060));
        CHECK(target.PixelAt(4, 0) == TileSheet::Remember(0x204060));
        CHECK(target.PixelAt(3, 1) == Back);
        CHECK(target.PixelAt(4, 1) == Back);
    }
    SECTION("negative and off-surface positions show the matching part")
    {
        sheet.Draw(&target, 0, -1, -1, Shade::Lit, true);
        CHECK(target.PixelAt(0, 0) == 0x000000);
        CHECK(target.PixelAt(1, 0) == Back);
        CHECK(target.PixelAt(0, 1) == Back);
        target.Fill(Back);
        sheet.Draw(&target, 0, 4, 3, Shade::Lit, false);
        CHECK(target.PixelAt(4, 3) == Key);
        CHECK(target.PixelAt(3, 3) == Back);
        target.Fill(Back);
        sheet.Draw(&target, 0, 4, 2, Shade::Lit, true);
        CHECK(target.PixelAt(4, 2) == Back);
        CHECK(target.PixelAt(4, 3) == 0xFF8000);
    }
    SECTION("outside, a missing target and a bad tile draw nothing")
    {
        const uint32_t clean = target.Checksum();
        sheet.Draw(&target, 0, 5, 0, Shade::Lit, false);
        sheet.Draw(&target, 0, -2, 0, Shade::Lit, false);
        sheet.Draw(&target, 0, 0, 4, Shade::Lit, false);
        sheet.Draw(&target, 0, 0, -2, Shade::Lit, false);
        sheet.Draw(&target, 0, INT32_MAX, INT32_MIN, Shade::Lit, false);
        sheet.Draw(&target, 2, 0, 0, Shade::Lit, false);
        sheet.Draw(&target, -1, 0, 0, Shade::Lit, false);
        sheet.Draw(nullptr, 0, 0, 0, Shade::Lit, false);
        TileSheet unloaded;
        unloaded.Draw(&target, 0, 0, 0, Shade::Lit, false);
        CHECK(target.Checksum() == clean);
    }
}

TEST_CASE("a known tile has a known checksum", "[graphics][tiles]")
{
    const char* const rows[] = {
        "..aa..", ".abba.", "abccba", "abccba", ".abba.", "..aa..",
    };
    const std::array<uint32_t, 3> colors = {0x102030, 0xC08020, 0xF0F0F0};
    TileSheet sheet;
    REQUIRE(sheet.Load(rows, 1, 6, colors.data(), 3) == TileStatus::Ok);

    Surface lit(6, 6);
    lit.Fill(0);
    sheet.Draw(&lit, 0, 0, 0, Shade::Lit, true);
    Surface remembered(6, 6);
    remembered.Fill(0);
    sheet.Draw(&remembered, 0, 0, 0, Shade::Remembered, true);

    uint32_t expected_lit = 2166136261U;
    uint32_t expected_remembered = 2166136261U;
    for (int32_t y = 0; y < 6; ++y)
    {
        for (int32_t x = 0; x < 6; ++x)
        {
            const char symbol = rows[y][x];
            const uint32_t color = symbol == '.' ? 0 : colors[static_cast<size_t>(symbol - 'a')];
            const uint32_t dimmed = symbol == '.' ? 0
                                                  : (((color >> 16 & 0xFF) * 3 / 8) << 16 |
                                                     ((color >> 8 & 0xFF) * 3 / 8) << 8 |
                                                     ((color & 0xFF) * 3 / 8 + 0x10));
            for (int shift = 0; shift < 32; shift += 8)
            {
                expected_lit = (expected_lit ^ ((color >> shift) & 0xFFU)) * 16777619U;
                expected_remembered =
                    (expected_remembered ^ ((dimmed >> shift) & 0xFFU)) * 16777619U;
            }
        }
    }
    CHECK(lit.Checksum() == expected_lit);
    CHECK(remembered.Checksum() == expected_remembered);
    CHECK(lit.Checksum() != remembered.Checksum());
    CHECK(lit.Checksum() == 1788453701U);
    CHECK(remembered.Checksum() == 2251193093U);
}

TEST_CASE("the sheet gfxcheck loads draws the picture its independent checksum names",
          "[graphics][tiles]")
{
    namespace Check = FeatherOS::User::GfxCheck;
    TileSheet sheet;
    REQUIRE(sheet.Load(Check::TileRows(), Check::TileCount, Check::TileSize, Check::TilePalette(),
                       Check::PaletteSize) == TileStatus::Ok);
    Surface picture(Check::TilePictureWidth, Check::TilePictureHeight);
    picture.Fill(0x102030);
    sheet.Draw(&picture, 0, 2, 2, Shade::Lit, true);
    sheet.Draw(&picture, 0, 12, 2, Shade::Remembered, true);
    sheet.Draw(&picture, 1, 22, 2, Shade::Lit, true);
    sheet.Draw(&picture, 1, 32, 2, Shade::Remembered, true);
    sheet.Draw(&picture, 1, -4, 12, Shade::Lit, false);
    sheet.Draw(&picture, 0, 36, 16, Shade::Remembered, true);
    CHECK(Check::ExpectedTileChecksum() == 1753845943U);
    CHECK(picture.Checksum() == Check::ExpectedTileChecksum());
}
