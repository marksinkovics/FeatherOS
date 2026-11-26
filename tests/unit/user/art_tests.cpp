#include <catch2/catch_test_macros.hpp>

#include "user/graphics/canvas.h"
#include "user/graphics/outline_renderer.h"
#include "user/graphics/surface.h"
#include "user/ui/art.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <utility>

namespace Graphics = FeatherOS::Graphics;
namespace UI = FeatherOS::UI;
using Graphics::Canvas;
using Graphics::Surface;
using UI::GlyphId;

namespace
{

constexpr uint32_t Black = 0xFF000000;
constexpr uint32_t White = 0xFFFFFFFF;
constexpr uint32_t BlackPixel = Black & Graphics::ColorMask;
constexpr uint32_t WhitePixel = White & Graphics::ColorMask;

void UseRealFile()
{
    REQUIRE(UI::LoadArt(FEATHEROS_OUTLINES_FILE) == Graphics::OutlineStatus::Ok);
}

Surface DrawOne(GlyphId glyph, int32_t size)
{
    Surface surface(size, size);
    surface.Fill(Black);
    Canvas canvas(surface);
    UI::DrawGlyph(canvas, glyph, {0, 0, size, size}, White);
    return surface;
}

} // namespace

TEST_CASE("the file holds every GlyphId, the feather and the wallpaper", "[art]")
{
    UseRealFile();
    const Graphics::Outlines& art = UI::Art();
    REQUIRE(art.Valid());
    CHECK(art.Count() ==
          static_cast<int32_t>(UI::GlyphCount + UI::PointerDrawingCount) + 2);
    for (uint32_t glyph = 0; glyph < UI::GlyphCount; ++glyph)
    {
        INFO(UI::GlyphDrawingNames[glyph]);
        const int32_t index = art.Find(UI::GlyphDrawingNames[glyph]);
        REQUIRE(index >= 0);
        CHECK(art.ViewBox(index).width == 24);
        CHECK(art.ViewBox(index).height == 24);
        CHECK(art.SinglePaint(index));
    }
    const int32_t feather = art.Find("feather");
    REQUIRE(feather >= 0);
    CHECK(art.ViewBox(feather).width == 64);
    CHECK_FALSE(art.SinglePaint(feather));
    const int32_t wallpaper = art.Find("wallpaper");
    REQUIRE(wallpaper >= 0);
    CHECK(art.ViewBox(wallpaper).width == 1600);
    CHECK(art.ViewBox(wallpaper).height == 1000);
    CHECK(art.ItemCount(wallpaper) == 5);
    CHECK_FALSE(art.SinglePaint(wallpaper));
}

TEST_CASE("GlyphId names its drawing", "[art]")
{
    CHECK(std::strcmp(UI::GlyphDrawingNames[static_cast<uint32_t>(GlyphId::Close)],
                      "glyph.close") == 0);
    CHECK(std::strcmp(UI::GlyphDrawingNames[static_cast<uint32_t>(GlyphId::Folder)],
                      "glyph.folder") == 0);
    CHECK(std::strcmp(UI::GlyphDrawingNames[UI::GlyphCount - 1], "glyph.arrow") == 0);
}

TEST_CASE("the close glyph at 16 px is symmetric about both diagonals", "[art]")
{
    UseRealFile();
    const Surface surface = DrawOne(GlyphId::Close, 16);
    int32_t ink = 0;
    for (int32_t y = 0; y < 16; ++y)
    {
        for (int32_t x = 0; x < 16; ++x)
        {
            INFO(x << "," << y);
            const uint32_t pixel = surface.PixelAt(x, y);
            CHECK(pixel == surface.PixelAt(y, x));
            CHECK(pixel == surface.PixelAt(15 - y, 15 - x));
            ink += pixel != BlackPixel;
        }
    }
    CHECK(ink > 20);
}

TEST_CASE("a glyph at 24 px keeps its ink inside the 24 unit box", "[art]")
{
    UseRealFile();
    constexpr int32_t Margin = 8;
    constexpr int32_t Size = 24;
    for (uint32_t glyph = 0; glyph < UI::GlyphCount; ++glyph)
    {
        INFO(UI::GlyphDrawingNames[glyph]);
        Surface surface(Size + 2 * Margin, Size + 2 * Margin);
        surface.Fill(Black);
        Canvas canvas(surface);
        Graphics::OutlinePaints paints;
        paints.current = White;
        canvas.DrawOutline(UI::Art(), UI::Art().Find(UI::GlyphDrawingNames[glyph]),
                           {Margin, Margin, Size, Size}, paints);
        int32_t inside = 0;
        int32_t outside = 0;
        int32_t edge = 0;
        for (int32_t y = 0; y < surface.Height(); ++y)
        {
            for (int32_t x = 0; x < surface.Width(); ++x)
            {
                const bool ink = surface.PixelAt(x, y) != BlackPixel;
                const bool in_box =
                    x >= Margin && x < Margin + Size && y >= Margin && y < Margin + Size;
                const bool on_edge = in_box && (x == Margin || x == Margin + Size - 1 ||
                                                y == Margin || y == Margin + Size - 1);
                inside += ink && in_box;
                outside += ink && !in_box;
                edge += ink && on_edge;
            }
        }
        CHECK(inside > 0);
        CHECK(outside == 0);
        CHECK(edge == 0);
    }
}

TEST_CASE("DrawGlyph through the cache equals drawing the outline", "[art]")
{
    UseRealFile();
    for (const int32_t size : {12, 16, 20, 24, 64})
    {
        const Surface cached = DrawOne(GlyphId::Gear, size);
        Surface direct(size, size);
        direct.Fill(Black);
        Canvas canvas(direct);
        Graphics::OutlinePaints paints;
        paints.current = White;
        canvas.DrawOutline(UI::Art(), UI::Art().Find("glyph.gear"), {0, 0, size, size}, paints);
        CHECK(cached.Checksum() == direct.Checksum());
        CHECK(DrawOne(GlyphId::Gear, size).Checksum() == cached.Checksum());
    }
}

TEST_CASE("a glyph takes the colour it is drawn in, blended by coverage", "[art]")
{
    UseRealFile();
    Surface surface(48, 48);
    surface.Fill(0x00102030);
    Canvas canvas(surface);
    UI::DrawGlyph(canvas, GlyphId::Plus, {0, 0, 48, 48}, 0xFF80C040);
    CHECK(surface.PixelAt(24, 24) == 0x0080C040);
    CHECK(surface.PixelAt(0, 0) == 0x00102030);
    const uint32_t edge = surface.PixelAt(22, 30);
    CHECK(edge != 0x0080C040);
    CHECK(edge != 0x00102030);
}

TEST_CASE("the feather draws its shaft in the second colour", "[art]")
{
    UseRealFile();
    Surface surface(64, 64);
    surface.Fill(Black);
    Canvas canvas(surface);
    UI::DrawFeather(canvas, {0, 0, 64, 64}, 0xFFE0E0E0, 0xFF204080);
    // The shaft crosses the vane near (29.5, 33.5); the quill lies at (11.5, 53).
    CHECK(surface.PixelAt(30, 33) == 0x00204080);
    CHECK(surface.PixelAt(11, 53) == 0x00E0E0E0);
    CHECK(surface.PixelAt(40, 30) == 0x00E0E0E0);
    CHECK(surface.PixelAt(22, 30) == 0x00E0E0E0);
    CHECK(surface.PixelAt(2, 2) == BlackPixel);
}

TEST_CASE("a missing outlines file draws an empty box", "[art]")
{
    CHECK(UI::LoadArt("/nonexistent/outlines.fvo") != Graphics::OutlineStatus::Ok);
    CHECK_FALSE(UI::Art().Valid());
    Surface surface(12, 12);
    surface.Fill(Black);
    Canvas canvas(surface);
    UI::DrawGlyph(canvas, GlyphId::Folder, {2, 2, 8, 8}, White);
    UI::DrawFeather(canvas, {2, 2, 8, 8}, White, White);
    for (int32_t y = 0; y < 12; ++y)
    {
        for (int32_t x = 0; x < 12; ++x)
        {
            const bool border = x >= 2 && x < 10 && y >= 2 && y < 10 &&
                                (x == 2 || x == 9 || y == 2 || y == 9);
            INFO(x << "," << y);
            CHECK(surface.PixelAt(x, y) == (border ? WhitePixel : BlackPixel));
        }
    }
    UseRealFile();
    CHECK(UI::Art().Valid());
}

TEST_CASE("a GlyphId out of range draws an empty box and an empty destination nothing", "[art]")
{
    UseRealFile();
    Surface surface(10, 10);
    surface.Fill(Black);
    Canvas canvas(surface);
    UI::DrawGlyph(canvas, static_cast<GlyphId>(200), {1, 1, 8, 8}, White);
    CHECK(surface.PixelAt(1, 1) == WhitePixel);
    CHECK(surface.PixelAt(4, 4) == BlackPixel);
    const uint32_t before = surface.Checksum();
    UI::DrawGlyph(canvas, GlyphId::Folder, {0, 0, 0, 5}, White);
    UI::DrawGlyph(canvas, GlyphId::Folder, {0, 0, 5, -3}, White);
    CHECK(surface.Checksum() == before);
}

TEST_CASE("a glyph too large for the cache is drawn straight from the outline", "[art]")
{
    UseRealFile();
    constexpr int32_t Size = 1536;
    Surface surface(Size, Size);
    surface.Fill(Black);
    Canvas canvas(surface);
    UI::DrawGlyph(canvas, GlyphId::Plus, {0, 0, Size, Size}, White);
    CHECK(surface.PixelAt(Size / 2, Size / 2) == WhitePixel);
    CHECK(surface.PixelAt(10, 10) == BlackPixel);
}

namespace
{

constexpr uint32_t TileBackground = 0x00102030;
constexpr uint32_t TileColor = 0xFF3D78B0;
constexpr uint32_t TilePixel = TileColor & Graphics::ColorMask;
constexpr int32_t TileSize = 44;
// (44 * 56 + 50) / 100 = 25 px glyph, at (44 - 25) / 2 = 9.
constexpr int32_t GlyphSize = 25;
constexpr int32_t GlyphOffset = 9;

Surface DrawTile(UI::AppTile tile, UI::TileStyle style, int32_t size = TileSize)
{
    Surface surface(size, size);
    surface.Fill(TileBackground);
    Canvas canvas(surface);
    UI::DrawAppTile(canvas, {0, 0, size, size}, tile, style);
    return surface;
}

UI::AppTile ColorTile(GlyphId glyph)
{
    UI::AppTile tile;
    tile.glyph = glyph;
    tile.color = TileColor;
    return tile;
}

bool InGlyphBox(int32_t x, int32_t y)
{
    return x >= GlyphOffset && x < GlyphOffset + GlyphSize && y >= GlyphOffset &&
           y < GlyphOffset + GlyphSize;
}

uint32_t Channel(uint32_t pixel, int32_t shift)
{
    return (pixel >> shift) & 0xFF;
}

int32_t Brightness(uint32_t pixel)
{
    return static_cast<int32_t>(Channel(pixel, 16) + Channel(pixel, 8) + Channel(pixel, 0));
}

} // namespace

TEST_CASE("an app tile at 44 px leaves its corners untouched", "[art]")
{
    UseRealFile();
    for (const UI::TileStyle style :
         {UI::TileStyle::Full, UI::TileStyle::Small, UI::TileStyle::Plain})
    {
        const Surface tile = DrawTile(ColorTile(GlyphId::Plus), style);
        CHECK(tile.PixelAt(0, 0) == TileBackground);
        CHECK(tile.PixelAt(TileSize - 1, 0) == TileBackground);
        CHECK(tile.PixelAt(0, TileSize - 1) == TileBackground);
        CHECK(tile.PixelAt(TileSize - 1, TileSize - 1) == TileBackground);
        CHECK(tile.PixelAt(TileSize / 2, 0) == TilePixel);
        CHECK(tile.PixelAt(0, TileSize / 2) == TilePixel);
    }
}

TEST_CASE("the fold darkens only the lower-right half, inside the rounded square", "[art]")
{
    UseRealFile();
    const UI::AppTile tile = ColorTile(GlyphId::Plus);
    const Surface full = DrawTile(tile, UI::TileStyle::Full);
    const Surface plain = DrawTile(tile, UI::TileStyle::Plain);
    int32_t darker = 0;
    for (int32_t y = 0; y < TileSize; ++y)
    {
        for (int32_t x = 0; x < TileSize; ++x)
        {
            INFO(x << "," << y);
            const int32_t d = x + y - TileSize;
            const uint32_t a = full.PixelAt(x, y);
            const uint32_t b = plain.PixelAt(x, y);
            if (InGlyphBox(x, y))
            {
                continue;
            }
            if (b == TileBackground)
            {
                CHECK(a == TileBackground);
            }
            else if (d <= -3)
            {
                CHECK(a == b);
            }
            else if (d >= 2)
            {
                if (b == TilePixel)
                {
                    // 16% black over the colour: each channel scaled by 214/255.
                    for (const int32_t shift : {16, 8, 0})
                    {
                        const int32_t expect = static_cast<int32_t>(Channel(b, shift) * 214 / 255);
                        CHECK(std::abs(static_cast<int32_t>(Channel(a, shift)) - expect) <= 1);
                    }
                    ++darker;
                }
                else
                {
                    CHECK(Brightness(a) <= Brightness(b));
                }
            }
        }
    }
    CHECK(darker > 300);
}

TEST_CASE("the hairline is a light line along the fold's diagonal", "[art]")
{
    UseRealFile();
    const UI::AppTile tile = ColorTile(GlyphId::Plus);
    const Surface full = DrawTile(tile, UI::TileStyle::Full);
    const Surface plain = DrawTile(tile, UI::TileStyle::Plain);
    // (38, 5) is on the diagonal x + y = 43, outside the glyph and the corner.
    REQUIRE_FALSE(InGlyphBox(38, 5));
    for (const int32_t shift : {16, 8, 0})
    {
        CHECK(Channel(full.PixelAt(38, 5), shift) > Channel(plain.PixelAt(38, 5), shift));
    }
    // Two pixels off the diagonal on either side there is none: the fold side is darker than the
    // colour, the other side is the colour.
    CHECK(full.PixelAt(36, 5) == TilePixel);
    CHECK(Brightness(full.PixelAt(41, 5)) < Brightness(TilePixel));
    // The line is symmetric about the diagonal's centre: x + y = 43 and the two pixels beside it.
    const int32_t before = Brightness(full.PixelAt(37, 5)) - Brightness(TilePixel);
    CHECK(before > 0);
    CHECK(Brightness(full.PixelAt(39, 5)) > Brightness(full.PixelAt(41, 5)));
}

TEST_CASE("the tile's glyph is white, 56% of the tile and centred in it", "[art]")
{
    UseRealFile();
    UI::AppTile tile = ColorTile(GlyphId::Plus);
    const Surface plain = DrawTile(tile, UI::TileStyle::Plain);
    Surface expected(TileSize, TileSize);
    expected.Fill(TileBackground);
    Canvas canvas(expected);
    canvas.FillRoundedRectangle({0, 0, TileSize, TileSize}, 10, TileColor);
    UI::DrawGlyph(canvas, GlyphId::Plus, {GlyphOffset, GlyphOffset, GlyphSize, GlyphSize}, White);
    CHECK(plain.Checksum() == expected.Checksum());

    const Surface bare = [&]
    {
        Surface surface(TileSize, TileSize);
        surface.Fill(TileBackground);
        Canvas bare_canvas(surface);
        bare_canvas.FillRoundedRectangle({0, 0, TileSize, TileSize}, 10, TileColor);
        return surface;
    }();
    int32_t left = TileSize;
    int32_t right = -1;
    int32_t top = TileSize;
    int32_t bottom = -1;
    for (int32_t y = 0; y < TileSize; ++y)
    {
        for (int32_t x = 0; x < TileSize; ++x)
        {
            if (plain.PixelAt(x, y) != bare.PixelAt(x, y))
            {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    }
    REQUIRE(right >= left);
    CHECK(std::abs(left + right - (TileSize - 1)) <= 1);
    CHECK(std::abs(top + bottom - (TileSize - 1)) <= 1);
    CHECK(right - left + 1 <= GlyphSize);
    CHECK(plain.PixelAt(TileSize / 2 - 1, TileSize / 2 - 1) == WhitePixel);
}

TEST_CASE("the small style has no fold and a 2.3 unit stroke, the plain one the normal stroke",
          "[art]")
{
    UseRealFile();
    const UI::AppTile tile = ColorTile(GlyphId::Plus);
    const Surface full = DrawTile(tile, UI::TileStyle::Full);
    const Surface small = DrawTile(tile, UI::TileStyle::Small);
    const Surface plain = DrawTile(tile, UI::TileStyle::Plain);

    Surface expected(TileSize, TileSize);
    expected.Fill(TileBackground);
    Canvas canvas(expected);
    canvas.FillRoundedRectangle({0, 0, TileSize, TileSize}, 10, TileColor);
    Graphics::OutlinePaints paints;
    paints.current = White;
    Graphics::OutlineOptions wide;
    wide.stroke_width = 589;
    canvas.DrawOutline(UI::Art(), UI::Art().Find("glyph.plus"),
                       {GlyphOffset, GlyphOffset, GlyphSize, GlyphSize}, paints, wide);
    CHECK(small.Checksum() == expected.Checksum());
    CHECK(small.Checksum() != plain.Checksum());
    CHECK(full.Checksum() != plain.Checksum());

    int32_t small_ink = 0;
    int32_t plain_ink = 0;
    for (int32_t y = 0; y < TileSize; ++y)
    {
        for (int32_t x = 0; x < TileSize; ++x)
        {
            small_ink += Brightness(small.PixelAt(x, y)) - Brightness(TilePixel);
            plain_ink += Brightness(plain.PixelAt(x, y)) - Brightness(TilePixel);
            if (!InGlyphBox(x, y))
            {
                CHECK(small.PixelAt(x, y) == plain.PixelAt(x, y));
            }
        }
    }
    CHECK(small_ink > plain_ink);
}

TEST_CASE("an app tile in a high-contrast theme is the surface with a 2 px text border", "[art]")
{
    UseRealFile();
    for (const UI::ThemeId id : {UI::ThemeId::ContrastDark, UI::ThemeId::ContrastLight})
    {
        const UI::Theme& theme = UI::BaseTheme(id);
        const UI::AppTile tile = UI::MakeAppTile(UI::AppId::Files, theme);
        REQUIRE(tile.high_contrast);
        const uint32_t surface_pixel = theme.surface & Graphics::ColorMask;
        const uint32_t text_pixel = theme.text & Graphics::ColorMask;
        const Surface full = DrawTile(tile, UI::TileStyle::Full);
        const Surface plain = DrawTile(tile, UI::TileStyle::Plain);
        CHECK(full.Checksum() == plain.Checksum());
        CHECK(full.PixelAt(0, 0) == TileBackground);
        CHECK(full.PixelAt(TileSize - 1, TileSize - 1) == TileBackground);
        CHECK(full.PixelAt(TileSize / 2, 0) == text_pixel);
        CHECK(full.PixelAt(TileSize / 2, 1) == text_pixel);
        CHECK(full.PixelAt(TileSize / 2, 2) == surface_pixel);
        CHECK(full.PixelAt(0, TileSize / 2) == text_pixel);
        CHECK(full.PixelAt(TileSize - 2, TileSize / 2) == text_pixel);
        CHECK(full.PixelAt(TileSize - 3, TileSize / 2) == surface_pixel);
        // No fold: the lower-right is the surface colour, outside the glyph.
        CHECK(full.PixelAt(36, 36) == surface_pixel);
        CHECK(full.PixelAt(38, 5) == surface_pixel);

        Surface expected(TileSize, TileSize);
        expected.Fill(TileBackground);
        Canvas canvas(expected);
        canvas.FillRoundedRectangle({0, 0, TileSize, TileSize}, 10, theme.surface);
        canvas.StrokeRoundedRectangle({0, 0, TileSize, TileSize}, 10, UI::ContrastBorderWidth,
                                      theme.text);
        UI::DrawGlyph(canvas, tile.glyph, {GlyphOffset, GlyphOffset, GlyphSize, GlyphSize},
                      theme.text);
        CHECK(full.Checksum() == expected.Checksum());
    }
    const UI::AppTile normal = UI::MakeAppTile(UI::AppId::Files, UI::BaseTheme(UI::ThemeId::Dusk));
    CHECK_FALSE(normal.high_contrast);
    CHECK(normal.color == UI::AppTileColor(UI::AppId::Files));
}

TEST_CASE("every app has a distinct tile glyph", "[art]")
{
    const UI::Theme& theme = UI::BaseTheme(UI::ThemeId::Dusk);
    CHECK(UI::MakeAppTile(UI::AppId::Files, theme).glyph == GlyphId::Folder);
    CHECK(UI::MakeAppTile(UI::AppId::Elements, theme).glyph == GlyphId::Shapes);
    for (uint32_t a = 0; a < UI::AppCount; ++a)
    {
        for (uint32_t b = a + 1; b < UI::AppCount; ++b)
        {
            CHECK(UI::MakeAppTile(static_cast<UI::AppId>(a), theme).glyph !=
                  UI::MakeAppTile(static_cast<UI::AppId>(b), theme).glyph);
        }
    }
}

TEST_CASE("an app tile of any size, position or rectangle stays inside its square", "[art]")
{
    UseRealFile();
    const UI::AppTile tile = ColorTile(GlyphId::Folder);
    for (const int32_t size : {1, 2, 3, 7, 16, 18, 24, 28, 64, 100})
    {
        for (const UI::TileStyle style :
             {UI::TileStyle::Full, UI::TileStyle::Small, UI::TileStyle::Plain})
        {
            Surface surface(size + 6, size + 6);
            surface.Fill(TileBackground);
            Canvas canvas(surface);
            UI::DrawAppTile(canvas, {3, 3, size, size}, tile, style);
            for (int32_t y = 0; y < surface.Height(); ++y)
            {
                for (int32_t x = 0; x < surface.Width(); ++x)
                {
                    if (x < 3 || y < 3 || x >= 3 + size || y >= 3 + size)
                    {
                        INFO(size << ": " << x << "," << y);
                        CHECK(surface.PixelAt(x, y) == TileBackground);
                    }
                }
            }
        }
    }
    // A wider rectangle gets the square, centred in it.
    Surface wide(60, 30);
    wide.Fill(TileBackground);
    Canvas canvas(wide);
    UI::DrawAppTile(canvas, {0, 0, 60, 30}, tile, UI::TileStyle::Plain);
    CHECK(wide.PixelAt(5, 15) == TileBackground);
    CHECK(wide.PixelAt(54, 15) == TileBackground);
    CHECK(wide.PixelAt(16, 15) == TilePixel);
    const uint32_t before = wide.Checksum();
    UI::DrawAppTile(canvas, {0, 0, 0, 30}, tile, UI::TileStyle::Full);
    UI::DrawAppTile(canvas, {0, 0, 30, -1}, tile, UI::TileStyle::Full);
    CHECK(wide.Checksum() == before);
}

namespace
{

using UI::PointerId;

constexpr uint32_t AccentA = 0xFFF0A840;
constexpr uint32_t AccentB = 0xFF2040E0;

struct PointerCase
{
    PointerId id;
    const char* drawing;
    // The hotspot at 24, 32 and 48 px.
    int32_t hotspot[3][2];
};

// 3 * 32 / 24 rounds 4.0 up from 3 and 2 * 32 / 24 = 2.67 to 3; 2 * 48 / 24 = 4; 12 scales exactly.
constexpr PointerCase PointerCases[] = {
    {PointerId::Arrow, "pointer.default", {{3, 2}, {4, 3}, {6, 4}}},
    {PointerId::Link, "pointer.pointer", {{3, 2}, {4, 3}, {6, 4}}},
    {PointerId::Text, "pointer.text", {{12, 12}, {16, 16}, {24, 24}}},
    {PointerId::Working, "pointer.progress.0", {{3, 2}, {4, 3}, {6, 4}}},
    {PointerId::Busy, "pointer.wait.0", {{12, 12}, {16, 16}, {24, 24}}},
    {PointerId::Move, "pointer.grab", {{12, 12}, {16, 16}, {24, 24}}},
    {PointerId::Moving, "pointer.grabbing", {{12, 12}, {16, 16}, {24, 24}}},
    {PointerId::ResizeVertical, "pointer.ns", {{12, 12}, {16, 16}, {24, 24}}},
    {PointerId::ResizeHorizontal, "pointer.ew", {{12, 12}, {16, 16}, {24, 24}}},
    {PointerId::ResizeNwse, "pointer.nwse", {{12, 12}, {16, 16}, {24, 24}}},
    {PointerId::ResizeNesw, "pointer.nesw", {{12, 12}, {16, 16}, {24, 24}}},
    {PointerId::NotAllowed, "pointer.no", {{12, 12}, {16, 16}, {24, 24}}},
    {PointerId::Precise, "pointer.cross", {{12, 12}, {16, 16}, {24, 24}}},
};
constexpr int32_t PointerSizes[3] = {24, 32, 48};

// The pointer drawn with its hotspot at (Origin, Origin) on a transparent premultiplied surface.
constexpr int32_t Origin = 60;

Surface DrawPointerOn(PointerId id, int32_t size, uint32_t accent, uint32_t frame)
{
    Surface surface(2 * Origin, 2 * Origin, Graphics::PixelFormat::PremultipliedArgb);
    surface.Fill(0);
    Canvas canvas(surface);
    UI::DrawPointer(canvas, id, Origin, Origin, size, accent, frame);
    return surface;
}

bool Same(const Surface& a, const Surface& b)
{
    return std::memcmp(a.Pixels(), b.Pixels(),
                       static_cast<size_t>(a.Width()) * static_cast<size_t>(a.Height()) * 4) == 0;
}

} // namespace

TEST_CASE("all 13 pointers exist with their hotspots at the three sizes", "[art]")
{
    UseRealFile();
    const Graphics::Outlines& art = UI::Art();
    static_assert(std::size(PointerCases) == UI::PointerCount);
    for (const PointerCase& pointer : PointerCases)
    {
        INFO(pointer.drawing);
        const int32_t index = art.Find(pointer.drawing);
        REQUIRE(index >= 0);
        CHECK(art.ViewBox(index).width == 24);
        CHECK(art.ViewBox(index).height == 24);
        CHECK_FALSE(art.SinglePaint(index));
        CHECK(art.Hotspot(index).present);
        const uint32_t id = static_cast<uint32_t>(pointer.id);
        CHECK(std::strcmp(UI::PointerDrawingNames[UI::PointerFirstDrawing[id]], pointer.drawing) ==
              0);
        for (int32_t which = 0; which < 3; ++which)
        {
            INFO("size " << PointerSizes[which]);
            const UI::PointerPoint hotspot = UI::PointerHotspot(pointer.id, PointerSizes[which]);
            CHECK(hotspot.x == pointer.hotspot[which][0]);
            CHECK(hotspot.y == pointer.hotspot[which][1]);
        }
    }
    CHECK(UI::PointerHotspot(static_cast<PointerId>(200), 24).x == 0);
    CHECK(UI::PointerHotspot(PointerId::Arrow, 0).x == 0);
}

TEST_CASE("a pointer is drawn with its hotspot at the given point", "[art]")
{
    UseRealFile();
    for (const PointerCase& pointer : PointerCases)
    {
        for (const int32_t size : PointerSizes)
        {
            INFO(pointer.drawing << " at " << size);
            const UI::PointerPoint hotspot = UI::PointerHotspot(pointer.id, size);
            const Surface surface = DrawPointerOn(pointer.id, size, AccentA, 0);
            int32_t left = surface.Width();
            int32_t top = surface.Height();
            int32_t right = -1;
            int32_t bottom = -1;
            for (int32_t y = 0; y < surface.Height(); ++y)
            {
                for (int32_t x = 0; x < surface.Width(); ++x)
                {
                    const uint32_t pixel = surface.PixelAt(x, y);
                    const uint32_t alpha = pixel >> 24;
                    // I3: no colour channel above the alpha.
                    CHECK(((pixel >> 16) & 0xFF) <= alpha);
                    CHECK(((pixel >> 8) & 0xFF) <= alpha);
                    CHECK((pixel & 0xFF) <= alpha);
                    if (alpha != 0)
                    {
                        left = std::min(left, x);
                        top = std::min(top, y);
                        right = std::max(right, x);
                        bottom = std::max(bottom, y);
                    }
                }
            }
            REQUIRE(right >= 0);
            CHECK(left >= Origin - hotspot.x);
            CHECK(top >= Origin - hotspot.y);
            CHECK(right < Origin - hotspot.x + size);
            CHECK(bottom < Origin - hotspot.y + size);
            // The arrows' tip is at their hotspot.
            if (hotspot.x < size / 2)
            {
                CHECK(left <= Origin + 1);
                CHECK(top <= Origin + 1);
            }
        }
    }
}

TEST_CASE("the accent slot changes only the accent parts", "[art]")
{
    UseRealFile();
    const struct
    {
        PointerId id;
        // The accent part's centre at 24 px, in 1/10 of a unit, and the radius it and its
        // outline stay within, in units.
        int32_t centre_x;
        int32_t centre_y;
        int32_t radius;
        bool accented;
    } cases[] = {
        {PointerId::Arrow, 0, 0, 0, false},
        {PointerId::Link, 180, 186, 4, true},
        {PointerId::Text, 0, 0, 0, false},
        {PointerId::Working, 178, 178, 4, true},
        {PointerId::Busy, 120, 120, 8, true},
        {PointerId::Move, 120, 120, 3, true},
        {PointerId::Moving, 120, 120, 4, true},
        {PointerId::ResizeVertical, 0, 0, 0, false},
        {PointerId::ResizeHorizontal, 0, 0, 0, false},
        {PointerId::ResizeNwse, 0, 0, 0, false},
        {PointerId::ResizeNesw, 0, 0, 0, false},
        {PointerId::NotAllowed, 0, 0, 0, false},
        {PointerId::Precise, 120, 120, 2, true},
    };
    for (const auto& entry : cases)
    {
        for (const int32_t size : PointerSizes)
        {
            INFO("pointer " << static_cast<int>(entry.id) << " at " << size);
            const UI::PointerPoint hotspot = UI::PointerHotspot(entry.id, size);
            const Surface a = DrawPointerOn(entry.id, size, AccentA, 0);
            const Surface b = DrawPointerOn(entry.id, size, AccentB, 0);
            if (!entry.accented)
            {
                CHECK(Same(a, b));
                continue;
            }
            const int32_t left = Origin - hotspot.x;
            const int32_t top = Origin - hotspot.y;
            int32_t differing = 0;
            for (int32_t y = 0; y < a.Height(); ++y)
            {
                for (int32_t x = 0; x < a.Width(); ++x)
                {
                    if (a.PixelAt(x, y) == b.PixelAt(x, y))
                    {
                        continue;
                    }
                    ++differing;
                    // Distance from the accent part's centre, in pixels, by the larger axis.
                    const int32_t dx = x - left - entry.centre_x * size / 240;
                    const int32_t dy = y - top - entry.centre_y * size / 240;
                    const int32_t reach = entry.radius * size / 24 + 1;
                    CHECK(std::abs(dx) <= reach);
                    CHECK(std::abs(dy) <= reach);
                }
            }
            CHECK(differing > 0);
        }
    }
}

TEST_CASE("the spinner's 8 frames differ and the ninth is the first", "[art]")
{
    UseRealFile();
    CHECK(UI::PointerFrameCount(PointerId::Working) == 8);
    CHECK(UI::PointerFrameCount(PointerId::Busy) == 8);
    CHECK(UI::PointerFrameCount(PointerId::Arrow) == 1);
    CHECK(UI::PointerFrameCount(static_cast<PointerId>(99)) == 1);
    for (const PointerId id : {PointerId::Working, PointerId::Busy})
    {
        for (const int32_t size : PointerSizes)
        {
            INFO("pointer " << static_cast<int>(id) << " at " << size);
            Surface frames[8] = {
                DrawPointerOn(id, size, AccentA, 0), DrawPointerOn(id, size, AccentA, 1),
                DrawPointerOn(id, size, AccentA, 2), DrawPointerOn(id, size, AccentA, 3),
                DrawPointerOn(id, size, AccentA, 4), DrawPointerOn(id, size, AccentA, 5),
                DrawPointerOn(id, size, AccentA, 6), DrawPointerOn(id, size, AccentA, 7),
            };
            for (int32_t a = 0; a < 8; ++a)
            {
                for (int32_t b = a + 1; b < 8; ++b)
                {
                    INFO("frames " << a << " and " << b);
                    CHECK_FALSE(Same(frames[a], frames[b]));
                }
            }
            CHECK(Same(frames[0], DrawPointerOn(id, size, AccentA, 8)));
            CHECK(Same(frames[3], DrawPointerOn(id, size, AccentA, 11)));
        }
    }
    CHECK(Same(DrawPointerOn(PointerId::Arrow, 32, AccentA, 0),
               DrawPointerOn(PointerId::Arrow, 32, AccentA, 5)));
}

TEST_CASE("Busy's frame four is its frame zero turned half way round", "[art]")
{
    UseRealFile();
    // Everything of Busy but the arc is a circle about the centre, so a half turn of the arc is
    // the whole picture reflected through the centre, up to the 1/256 rounding of its points.
    const int32_t size = 48;
    const Surface a = DrawPointerOn(PointerId::Busy, size, AccentA, 0);
    const Surface b = DrawPointerOn(PointerId::Busy, size, AccentA, 4);
    const UI::PointerPoint hotspot = UI::PointerHotspot(PointerId::Busy, size);
    const int32_t left = Origin - hotspot.x;
    const int32_t top = Origin - hotspot.y;
    int32_t differing = 0;
    for (int32_t y = 0; y < size; ++y)
    {
        for (int32_t x = 0; x < size; ++x)
        {
            const uint32_t turned = a.PixelAt(left + size - 1 - x, top + size - 1 - y);
            const uint32_t pixel = b.PixelAt(left + x, top + y);
            differing += pixel != a.PixelAt(left + x, top + y);
            for (const int32_t shift : {0, 8, 16, 24})
            {
                const int32_t gap = static_cast<int32_t>((pixel >> shift) & 0xFF) -
                                    static_cast<int32_t>((turned >> shift) & 0xFF);
                INFO(x << "," << y);
                CHECK(std::abs(gap) <= 2);
            }
        }
    }
    CHECK(differing > 100);
}

TEST_CASE("a pointer from a missing file draws an empty box at its point", "[art]")
{
    CHECK(UI::LoadArt("/nonexistent/outlines.fvo") != Graphics::OutlineStatus::Ok);
    CHECK(UI::PointerHotspot(PointerId::Arrow, 24).x == 0);
    Surface surface(40, 40, Graphics::PixelFormat::PremultipliedArgb);
    surface.Fill(0);
    Canvas canvas(surface);
    UI::DrawPointer(canvas, PointerId::Arrow, 5, 6, 10, AccentA, 0);
    CHECK((surface.PixelAt(5, 6) >> 24) == 255);
    CHECK(surface.PixelAt(7, 8) == 0);
    UI::DrawPointer(canvas, static_cast<PointerId>(99), 20, 20, 10, AccentA, 0);
    UI::DrawPointer(canvas, PointerId::Arrow, 20, 20, 0, AccentA, 0);
    CHECK(surface.PixelAt(20, 20) == 0);
    UseRealFile();
}

namespace
{

struct WallpaperCase
{
    int32_t width;
    int32_t height;
    Graphics::Rectangle drawn;
};

// Where the 1600 x 1000 drawing lands, worked out by hand: the side that limits the scale is the
// surface's own, the other rounds up and the overhang is cut half to each side.
const WallpaperCase WallpaperCases[] = {
    {1024, 768, {-102, 0, 1229, 768}},
    {1920, 1080, {0, -60, 1920, 1200}},
    {800, 600, {-80, 0, 960, 600}},
};

Surface WallpaperOn(int32_t width, int32_t height, const UI::Theme& theme)
{
    Surface surface(width, height);
    surface.Fill(0x00FF00FF);
    UI::DrawWallpaper(&surface, theme);
    return surface;
}

Graphics::Color Opaque(Graphics::Color color)
{
    return color | 0xFF000000U;
}

int32_t Channel8(uint32_t pixel, int shift)
{
    return static_cast<int32_t>((pixel >> shift) & 0xFF);
}

} // namespace

TEST_CASE("the wallpaper covers the surface at the three sizes", "[art]")
{
    UseRealFile();
    const UI::Theme& theme = UI::BaseTheme(UI::ThemeId::Dusk);
    for (const WallpaperCase& entry : WallpaperCases)
    {
        INFO(entry.width << "x" << entry.height);
        const Surface surface = WallpaperOn(entry.width, entry.height, theme);
        // The ridges end on the drawing's bottom edge: when the drawing is as tall as the surface
        // the whole last row is the lowest ridge, which only a cover reaches at both sides.
        const int32_t last = entry.height - 1;
        if (entry.drawn.height == entry.height)
        {
            for (int32_t x = 0; x < entry.width; ++x)
            {
                CHECK((surface.PixelAt(x, last) & Graphics::ColorMask) ==
                      (theme.wallpaper_3 & Graphics::ColorMask));
            }
        }
        // Drawn by hand into a desk-filled surface, the drawing equals the wallpaper.
        Surface expected(entry.width, entry.height);
        expected.Fill(theme.desk);
        Canvas canvas(expected);
        Graphics::OutlinePaints paints;
        paints.slots[0] = theme.wallpaper_feather;
        paints.slots[1] = theme.desk;
        paints.slots[2] = theme.wallpaper_1;
        paints.slots[3] = theme.wallpaper_2;
        paints.slots[4] = theme.wallpaper_3;
        const Graphics::Rectangle& drawn = entry.drawn;
        canvas.DrawOutline(UI::Art(), UI::Art().Find("wallpaper"), drawn, paints);
        CHECK(surface.Checksum() == expected.Checksum());
        // No pixel keeps the magenta the surface started with.
        for (int32_t y = 0; y < entry.height; ++y)
        {
            for (int32_t x = 0; x < entry.width; ++x)
            {
                REQUIRE((surface.PixelAt(x, y) & Graphics::ColorMask) != 0x00FF00FFU);
            }
        }
    }
}

TEST_CASE("the wallpaper's pixels have pinned checksums", "[art]")
{
    UseRealFile();
    const UI::Theme& theme = UI::BaseTheme(UI::ThemeId::Dusk);
    constexpr uint32_t Pinned[] = {0x7CAB6496, 0x2916538E, 0x0C644D18};
    static_assert(std::size(Pinned) == std::size(WallpaperCases));
    size_t index = 0;
    for (const WallpaperCase& entry : WallpaperCases)
    {
        INFO(entry.width << "x" << entry.height);
        CHECK(WallpaperOn(entry.width, entry.height, theme).Checksum() == Pinned[index++]);
    }
}

TEST_CASE("the wallpaper paints the feather tint, the ridges and the desk from the theme", "[art]")
{
    UseRealFile();
    for (const UI::ThemeId id : {UI::ThemeId::Dusk, UI::ThemeId::Airy, UI::ThemeId::ContrastLight})
    {
        const UI::Theme& theme = UI::BaseTheme(id);
        INFO(theme.name);
        // At 1600 x 1000 one view unit is one pixel.
        const Surface surface = WallpaperOn(1600, 1000, theme);
        const auto desk = theme.desk & Graphics::ColorMask;
        CHECK((surface.PixelAt(50, 300) & Graphics::ColorMask) == desk);
        CHECK((surface.PixelAt(800, 750) & Graphics::ColorMask) ==
              (theme.wallpaper_1 & Graphics::ColorMask));
        CHECK((surface.PixelAt(800, 850) & Graphics::ColorMask) ==
              (theme.wallpaper_2 & Graphics::ColorMask));
        CHECK((surface.PixelAt(800, 990) & Graphics::ColorMask) ==
              (theme.wallpaper_3 & Graphics::ColorMask));
        // Inside the vane the tint is blended over the desk by its alpha.
        const uint32_t alpha = theme.wallpaper_feather >> 24;
        const uint32_t pixel = surface.PixelAt(1000, 300);
        for (const int shift : {0, 8, 16})
        {
            const int32_t under = Channel8(theme.desk, shift);
            const int32_t over = Channel8(theme.wallpaper_feather, shift);
            const int32_t want = under + (over - under) * static_cast<int32_t>(alpha) / 255;
            CHECK(std::abs(Channel8(pixel, shift) - want) <= 1);
        }
    }
}

TEST_CASE("the wallpaper is drawn only on a valid surface and without the file only as the desk",
          "[art]")
{
    UseRealFile();
    const UI::Theme& theme = UI::BaseTheme(UI::ThemeId::Dusk);
    UI::DrawWallpaper(nullptr, theme);
    Surface empty(0, 0);
    UI::DrawWallpaper(&empty, theme);
    CHECK(UI::LoadArt("/nonexistent/outlines.fvo") != Graphics::OutlineStatus::Ok);
    const Surface plain = WallpaperOn(64, 40, theme);
    for (int32_t y = 0; y < 40; ++y)
    {
        for (int32_t x = 0; x < 64; ++x)
        {
            REQUIRE((plain.PixelAt(x, y) & Graphics::ColorMask) ==
                    (theme.desk & Graphics::ColorMask));
        }
    }
    UseRealFile();
}

TEST_CASE("a surface of an odd or extreme shape still takes the wallpaper", "[art]")
{
    UseRealFile();
    const UI::Theme& theme = UI::BaseTheme(UI::ThemeId::Dusk);
    for (const auto& [width, height] : {std::pair{1, 1}, std::pair{3, 2000}, std::pair{2000, 3}})
    {
        INFO(width << "x" << height);
        const Surface surface = WallpaperOn(width, height, theme);
        CHECK(surface.Valid());
        CHECK((surface.PixelAt(0, 0) & Graphics::ColorMask) != 0x00FF00FFU);
    }
}

TEST_CASE("ShadowFor returns the same shadow for a role, made from its spec", "[art]")
{
    for (uint32_t role = 0; role < UI::ShadowRoleCount; ++role)
    {
        const auto id = static_cast<UI::ShadowRole>(role);
        INFO("role " << role);
        const Graphics::NineSlice& first = UI::ShadowFor(id);
        CHECK(&UI::ShadowFor(id) == &first);
        const UI::ShadowSpec& spec = UI::ShadowSpecOf(id);
        const Graphics::NineSlice made = Graphics::MakeShadow(spec.corner_radius, spec.blur);
        CHECK(first.left == made.left);
        CHECK(first.top == made.top);
        CHECK(first.right == made.right);
        CHECK(first.bottom == made.bottom);
        REQUIRE(first.mask.Width() == made.mask.Width());
        REQUIRE(first.mask.Height() == made.mask.Height());
        CHECK(std::memcmp(first.mask.Data(), made.mask.Data(),
                          static_cast<size_t>(made.mask.Width() * made.mask.Height())) == 0);
        for (uint32_t other = role + 1; other < UI::ShadowRoleCount; ++other)
        {
            CHECK(&UI::ShadowFor(static_cast<UI::ShadowRole>(other)) != &first);
        }
    }
    CHECK(&UI::ShadowFor(static_cast<UI::ShadowRole>(200)) ==
          &UI::ShadowFor(UI::ShadowRole::WindowUnfocused));
}
