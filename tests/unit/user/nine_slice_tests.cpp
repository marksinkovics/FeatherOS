#include <catch2/catch_test_macros.hpp>

#include "user/graphics/canvas.h"
#include "user/graphics/color.h"
#include "user/graphics/corner_mask.h"
#include "user/graphics/mask.h"
#include "user/graphics/nine_slice.h"
#include "user/graphics/surface.h"
#include "user/ui/tokens.h"

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <random>
#include <vector>

namespace Graphics = FeatherOS::Graphics;
namespace UI = FeatherOS::UI;
using Graphics::Canvas;
using Graphics::Mask;
using Graphics::NineSlice;
using Graphics::PixelFormat;
using Graphics::Rectangle;
using Graphics::Surface;

namespace
{

uint64_t Total(const Mask& mask)
{
    uint64_t total = 0;
    for (int32_t y = 0; y < mask.Height(); ++y)
    {
        for (int32_t x = 0; x < mask.Width(); ++x)
        {
            total += mask.CoverageAt(x, y);
        }
    }
    return total;
}

std::vector<uint32_t> PixelsOf(const Surface& surface)
{
    return std::vector<uint32_t>(surface.Pixels(),
                                 surface.Pixels() + static_cast<size_t>(surface.Width()) *
                                                        static_cast<size_t>(surface.Height()));
}

// White at full alpha over black gives the coverage itself in every channel.
uint32_t Grey(uint32_t coverage)
{
    return coverage << 16 | coverage << 8 | coverage;
}

Surface DrawShadow(const NineSlice& slice, int32_t width, int32_t height, bool skip_centre = false)
{
    Surface surface(width, height);
    Canvas(surface).DrawNineSlice(slice, {0, 0, width, height}, Graphics::Rgb(255, 255, 255),
                                  skip_centre);
    return surface;
}

Mask Patterned(int32_t width, int32_t height)
{
    Mask mask(width, height);
    for (int32_t y = 0; y < height; ++y)
    {
        for (int32_t x = 0; x < width; ++x)
        {
            mask.SetCoverage(x, y, static_cast<uint8_t>(1 + (x * 29 + y * 13 + x * y) % 255));
        }
    }
    return mask;
}

} // namespace

TEST_CASE("The 8 token shadows are symmetric, fall away from the box and keep their total",
          "[nine_slice]")
{
    for (uint32_t role = 0; role < UI::ShadowRoleCount; ++role)
    {
        const UI::ShadowSpec& spec = UI::ShadowSpecOf(static_cast<UI::ShadowRole>(role));
        const NineSlice shadow = Graphics::MakeShadow(spec.corner_radius, spec.blur);
        const Mask& mask = shadow.mask;
        const int32_t spread = Graphics::ShadowSpread(spec.blur);
        REQUIRE(mask.Valid());
        CHECK(mask.Width() == mask.Height());
        CHECK(shadow.left == spec.corner_radius + 2 * spread);
        CHECK(mask.Width() == 2 * shadow.left + 1);
        const int32_t last = mask.Width() - 1;
        for (int32_t y = 0; y <= last; ++y)
        {
            for (int32_t x = 0; x <= last; ++x)
            {
                REQUIRE(mask.CoverageAt(x, y) == mask.CoverageAt(last - x, y));
                REQUIRE(mask.CoverageAt(x, y) == mask.CoverageAt(x, last - y));
            }
        }
        // From the middle outward, along a row and along the diagonal, coverage never rises.
        const int32_t middle = last / 2;
        for (int32_t i = middle; i > 0; --i)
        {
            REQUIRE(mask.CoverageAt(i - 1, middle) <= mask.CoverageAt(i, middle));
            REQUIRE(mask.CoverageAt(i - 1, i - 1) <= mask.CoverageAt(i, i));
        }
        CHECK(mask.CoverageAt(0, 0) == 0);
        CHECK(mask.CoverageAt(middle, middle) == 255);

        const Mask sharp = Graphics::MakeShadowMask(mask.Width() - 2 * spread,
                                                    mask.Height() - 2 * spread,
                                                    spec.corner_radius, 0);
        const uint64_t before = Total(sharp);
        const uint64_t after = Total(mask);
        const uint64_t difference = before > after ? before - after : after - before;
        CHECK(difference * 100 <= before);
    }
}

TEST_CASE("A blur of 0 is the rounded rectangle's own mask", "[nine_slice]")
{
    for (int32_t radius : {0, 1, 4, 10})
    {
        const NineSlice shadow = Graphics::MakeShadow(radius, 0);
        const int32_t size = 2 * radius + 1;
        REQUIRE(shadow.mask.Width() == size);
        REQUIRE(shadow.mask.Height() == size);
        CHECK(shadow.left == radius);
        CHECK(shadow.right == radius);
        CHECK(shadow.top == radius);
        CHECK(shadow.bottom == radius);
        std::vector<uint8_t> row(static_cast<size_t>(radius > 0 ? radius : 1));
        for (int32_t y = 0; y < size; ++y)
        {
            const int32_t edge = y < radius ? y : (y > radius ? size - 1 - y : radius);
            if (edge < radius)
            {
                Graphics::ComputeCornerRow(radius, edge, row.data());
            }
            for (int32_t x = 0; x < size; ++x)
            {
                const int32_t across = x < radius ? x : (x > radius ? size - 1 - x : radius);
                const uint8_t expected = edge < radius && across < radius
                                             ? row[static_cast<size_t>(across)]
                                             : 255;
                REQUIRE(shadow.mask.CoverageAt(x, y) == expected);
            }
        }
    }
}

TEST_CASE("A shadow's radius and blur are cut to their limits", "[nine_slice]")
{
    const NineSlice big = Graphics::MakeShadow(1 << 30, 1 << 30);
    REQUIRE(big.mask.Valid());
    const int32_t inset = Graphics::MaximumShadowRadius +
                          2 * Graphics::ShadowSpread(Graphics::MaximumShadowBlur);
    CHECK(big.left == inset);
    CHECK(big.mask.Width() == 2 * inset + 1);
    CHECK(Graphics::ShadowSpread(std::numeric_limits<int32_t>::max()) ==
          Graphics::ShadowSpread(Graphics::MaximumShadowBlur));

    const NineSlice negative = Graphics::MakeShadow(-5, -5);
    CHECK(negative.mask.Width() == 1);
    CHECK(negative.mask.CoverageAt(0, 0) == 255);
    CHECK(Graphics::ShadowSpread(-1) == 0);

    CHECK_FALSE(Graphics::MakeShadowMask(0, 5, 2, 4).Valid());
    CHECK_FALSE(Graphics::MakeShadowMask(5, -1, 2, 4).Valid());
    CHECK_FALSE(Graphics::MakeShadowMask(Graphics::MaximumDimension, 5, 2, 4).Valid());
}

TEST_CASE("ShadowBounds grows the box by the spread and moves it down", "[nine_slice]")
{
    const Rectangle bounds = Graphics::ShadowBounds({100, 50, 300, 200}, 10, 26);
    const int32_t spread = Graphics::ShadowSpread(26);
    CHECK(spread == 39);
    CHECK(bounds.x == 100 - spread);
    CHECK(bounds.y == 50 - spread + 10);
    CHECK(bounds.width == 300 + 2 * spread);
    CHECK(bounds.height == 200 + 2 * spread);

    const Rectangle none = Graphics::ShadowBounds({0, 0, 10, 10}, 0, 0);
    CHECK(none.x == 0);
    CHECK(none.width == 10);

    const Rectangle extreme = Graphics::ShadowBounds(
        {std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max(),
         std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::max()},
        std::numeric_limits<int32_t>::max(), 26);
    CHECK(extreme.x == std::numeric_limits<int32_t>::min());
    CHECK(extreme.y == std::numeric_limits<int32_t>::max());
    CHECK(extreme.width == std::numeric_limits<int32_t>::max());
}

TEST_CASE("A nine-slice drawn at the mask's size equals DrawMask", "[nine_slice]")
{
    // The centre is one pixel wide and high, as in a shadow, so its repeated middle is itself.
    const NineSlice slices[] = {{Patterned(12, 9), 5, 4, 6, 4}, Graphics::MakeShadow(6, 12)};
    for (const NineSlice& slice : slices)
    {
        const int32_t width = slice.mask.Width();
        const int32_t height = slice.mask.Height();
        for (const PixelFormat format : {PixelFormat::Xrgb, PixelFormat::PremultipliedArgb})
        {
            for (const Graphics::Color color : {0xFFFFFFFFU, 0x80204060U, 0x01FF0000U})
            {
                Surface sliced(width + 10, height + 8, format);
                Surface masked(width + 10, height + 8, format);
                Canvas(sliced).FillRectangle({0, 0, width + 10, height + 8}, 0xFF5090C0);
                Canvas(masked).FillRectangle({0, 0, width + 10, height + 8}, 0xFF5090C0);
                Canvas(sliced).DrawNineSlice(slice, {4, 3, width, height}, color, false);
                Canvas(masked).DrawMask(slice.mask, 4, 3, color);
                REQUIRE(PixelsOf(sliced) == PixelsOf(masked));
            }
        }
    }
}

TEST_CASE("A stretched nine-slice keeps its corners and repeats its middle row and column",
          "[nine_slice]")
{
    const NineSlice slice{Patterned(21, 17), 5, 4, 6, 3};
    for (const Rectangle size : {Rectangle{0, 0, 21, 40}, {0, 0, 60, 17}, {0, 0, 33, 29}})
    {
        const Surface surface = DrawShadow(slice, size.width, size.height);
        const Mask& mask = slice.mask;
        for (int32_t y = 0; y < size.height; ++y)
        {
            for (int32_t x = 0; x < size.width; ++x)
            {
                const int32_t sx = x < slice.left
                                       ? x
                                       : (x >= size.width - slice.right
                                              ? mask.Width() - (size.width - x)
                                              : slice.left +
                                                    (mask.Width() - slice.left - slice.right) / 2);
                const int32_t sy = y < slice.top
                                       ? y
                                       : (y >= size.height - slice.bottom
                                              ? mask.Height() - (size.height - y)
                                              : slice.top + (mask.Height() - slice.top -
                                                             slice.bottom) / 2);
                REQUIRE(surface.PixelAt(x, y) == Grey(mask.CoverageAt(sx, sy)));
            }
        }
    }
}

TEST_CASE("A stretched shadow equals the blur of the larger box", "[nine_slice]")
{
    struct Case
    {
        int32_t radius;
        int32_t blur;
    };
    for (const Case c : {Case{6, 6}, Case{4, 18}, Case{10, 16}, Case{0, 12}, Case{6, 26}})
    {
        const NineSlice slice = Graphics::MakeShadow(c.radius, c.blur);
        const int32_t spread = Graphics::ShadowSpread(c.blur);
        const int32_t smallest = slice.mask.Width();
        for (const Rectangle size :
             {Rectangle{0, 0, smallest, smallest}, {0, 0, smallest + 7, smallest + 31},
              {0, 0, smallest + 90, smallest + 3}})
        {
            const Surface surface = DrawShadow(slice, size.width, size.height);
            const Mask direct = Graphics::MakeShadowMask(size.width - 2 * spread,
                                                         size.height - 2 * spread, c.radius, c.blur);
            REQUIRE(direct.Width() == size.width);
            REQUIRE(direct.Height() == size.height);
            for (int32_t y = 0; y < size.height; ++y)
            {
                for (int32_t x = 0; x < size.width; ++x)
                {
                    REQUIRE(surface.PixelAt(x, y) == Grey(direct.CoverageAt(x, y)));
                }
            }
        }
    }
}

TEST_CASE("skip_centre leaves the inner rectangle untouched", "[nine_slice]")
{
    const NineSlice slice = Graphics::MakeShadow(6, 12);
    const int32_t size = slice.mask.Width() + 20;
    Surface skipped(size, size);
    Surface full(size, size);
    skipped.Fill(0x123456);
    full.Fill(0x123456);
    const Rectangle outer{0, 0, size, size};
    Canvas(skipped).DrawNineSlice(slice, outer, 0xFFFFFFFF, true);
    Canvas(full).DrawNineSlice(slice, outer, 0xFFFFFFFF, false);
    bool outside_differs = false;
    for (int32_t y = 0; y < size; ++y)
    {
        for (int32_t x = 0; x < size; ++x)
        {
            const bool inside = x >= slice.left && x < size - slice.right && y >= slice.top &&
                                y < size - slice.bottom;
            if (inside)
            {
                REQUIRE(skipped.PixelAt(x, y) == 0x123456);
            }
            else
            {
                REQUIRE(skipped.PixelAt(x, y) == full.PixelAt(x, y));
                outside_differs = outside_differs || skipped.PixelAt(x, y) != 0x123456;
            }
        }
    }
    CHECK(outside_differs);
}

TEST_CASE("A nine-slice drawn smaller than its insets shares the size between them",
          "[nine_slice]")
{
    const NineSlice slice{Patterned(21, 17), 5, 4, 6, 3};
    const Surface surface = DrawShadow(slice, 4, 3);
    // Widths 5 : 6 of 4 give 1 and 3; heights 4 : 3 of 3 give 1 and 2.
    const Mask& mask = slice.mask;
    CHECK(surface.PixelAt(0, 0) == Grey(mask.CoverageAt(0, 0)));
    CHECK(surface.PixelAt(3, 2) == Grey(mask.CoverageAt(20, 16)));
    CHECK(surface.PixelAt(1, 0) == Grey(mask.CoverageAt(18, 0)));
    CHECK(surface.PixelAt(0, 2) == Grey(mask.CoverageAt(0, 16)));

    const Surface one = DrawShadow(slice, 1, 1);
    CHECK(one.PixelAt(0, 0) == Grey(mask.CoverageAt(20, 16)));
}

TEST_CASE("Degenerate nine-slices and rectangles draw nothing", "[nine_slice]")
{
    const NineSlice slice = Graphics::MakeShadow(4, 8);
    Surface surface(40, 40);
    surface.Fill(0x102030);
    const std::vector<uint32_t> before = PixelsOf(surface);
    Canvas canvas(surface);
    canvas.DrawNineSlice(slice, {0, 0, 0, 40}, 0xFFFFFFFF, false);
    canvas.DrawNineSlice(slice, {0, 0, 40, -3}, 0xFFFFFFFF, false);
    canvas.DrawNineSlice(slice, {0, 0, 40, 40}, 0x00FFFFFF, false);
    canvas.DrawNineSlice({Mask(0, 0), 0, 0, 0, 0}, {0, 0, 40, 40}, 0xFFFFFFFF, false);
    canvas.DrawNineSlice({Mask(4, 4), 3, 0, 3, 0}, {0, 0, 40, 40}, 0xFFFFFFFF, false);
    canvas.DrawNineSlice({Mask(4, 4), -1, 0, 1, 0}, {0, 0, 40, 40}, 0xFFFFFFFF, false);
    CHECK(PixelsOf(surface) == before);
}

TEST_CASE("A nine-slice never writes outside the clip", "[nine_slice]")
{
    std::mt19937 random(37);
    const NineSlice slices[] = {Graphics::MakeShadow(6, 12), {Patterned(9, 7), 2, 3, 4, 1},
                                Graphics::MakeShadow(0, 0)};
    for (const PixelFormat format : {PixelFormat::Xrgb, PixelFormat::PremultipliedArgb})
    {
        for (int round = 0; round < 400; ++round)
        {
            Surface surface(48, 40, format);
            Canvas canvas(surface);
            canvas.FillRectangle({0, 0, 48, 40}, 0xFF446688);
            const std::vector<uint32_t> before = PixelsOf(surface);
            auto pick = [&](int32_t extent) {
                const uint32_t kind = random() % 8;
                if (kind == 0)
                {
                    return std::numeric_limits<int32_t>::min() + static_cast<int32_t>(random() % 5);
                }
                if (kind == 1)
                {
                    return std::numeric_limits<int32_t>::max() - static_cast<int32_t>(random() % 5);
                }
                return static_cast<int32_t>(random() % (2 * extent + 20)) - extent - 10;
            };
            const Rectangle clip{pick(48), pick(40), static_cast<int32_t>(random() % 60),
                                 static_cast<int32_t>(random() % 50)};
            canvas.Translate(static_cast<int32_t>(random() % 20) - 10,
                             static_cast<int32_t>(random() % 20) - 10);
            canvas.ClipTo(clip);
            const Rectangle visible = canvas.Clip();
            const Rectangle outer{pick(60), pick(50), static_cast<int32_t>(random() % 90),
                                  static_cast<int32_t>(random() % 80)};
            canvas.DrawNineSlice(slices[random() % 3], outer,
                                 0x40000000U | static_cast<uint32_t>(random() % 0xFFFFFF),
                                 random() % 2 == 0);
            for (int32_t y = 0; y < 40; ++y)
            {
                for (int32_t x = 0; x < 48; ++x)
                {
                    const bool inside = x >= visible.x && x < visible.x + visible.width &&
                                        y >= visible.y && y < visible.y + visible.height;
                    if (!inside)
                    {
                        REQUIRE(surface.PixelAt(x, y) == before[static_cast<size_t>(y) * 48 + x]);
                    }
                }
            }
        }
    }
}
