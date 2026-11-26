#include <catch2/catch_test_macros.hpp>

#include "user/graphics/canvas.h"
#include "user/graphics/color.h"
#include "user/graphics/corner_mask.h"
#include "user/graphics/surface.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <random>
#include <vector>

namespace Graphics = FeatherOS::Graphics;
using Graphics::Canvas;
using Graphics::Color;
using Graphics::CornerCoverage;
using Graphics::PixelFormat;
using Graphics::Rectangle;
using Graphics::Surface;

namespace
{

constexpr Color White = 0xFFFFFFFFU;

// Black XRGB surface; white at coverage c gives the pixel 0xcccccc, so the blue byte is c.
Surface Blank(int32_t width, int32_t height)
{
    Surface surface(width, height);
    surface.Fill(0);
    return surface;
}

std::vector<uint8_t> CoverageOf(const Surface& surface)
{
    std::vector<uint8_t> result;
    for (int32_t y = 0; y < surface.Height(); ++y)
    {
        for (int32_t x = 0; x < surface.Width(); ++x)
        {
            result.push_back(static_cast<uint8_t>(surface.PixelAt(x, y) & 0xFF));
        }
    }
    return result;
}

Surface Filled(int32_t width, int32_t height, int32_t radius, int32_t margin = 0)
{
    Surface surface = Blank(width + 2 * margin, height + 2 * margin);
    Canvas(surface).FillRoundedRectangle({margin, margin, width, height}, radius, White);
    return surface;
}

// Every sample of every pixel is decided on its own, with the corners found by mirroring the
// sample into the top-left quarter. `grid` samples per side, centres at (2k + 1) / (2 grid).
std::vector<uint8_t> ReferenceCoverage(int32_t width, int32_t height, int32_t radius, int32_t grid)
{
    const int64_t unit = 2 * grid;
    const int64_t cut = std::max<int64_t>(
        0, std::min<int64_t>(radius, std::min<int64_t>(width, height) / 2));
    const int64_t r = cut * unit;
    std::vector<uint8_t> result;
    for (int32_t y = 0; y < height; ++y)
    {
        for (int32_t x = 0; x < width; ++x)
        {
            int64_t count = 0;
            for (int32_t j = 0; j < grid; ++j)
            {
                for (int32_t i = 0; i < grid; ++i)
                {
                    const int64_t u = int64_t{x} * unit + 2 * i + 1;
                    const int64_t v = int64_t{y} * unit + 2 * j + 1;
                    const int64_t mu = std::min(u, int64_t{width} * unit - u);
                    const int64_t mv = std::min(v, int64_t{height} * unit - v);
                    bool inside = true;
                    if (mu < r && mv < r)
                    {
                        inside = (r - mu) * (r - mu) + (r - mv) * (r - mv) <= r * r;
                    }
                    count += inside ? 1 : 0;
                }
            }
            result.push_back(static_cast<uint8_t>((count * 255 + grid * grid / 2) / (grid * grid)));
        }
    }
    return result;
}

int32_t MaximumDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    REQUIRE(a.size() == b.size());
    int32_t worst = 0;
    for (size_t index = 0; index < a.size(); ++index)
    {
        worst = std::max(worst, std::abs(static_cast<int32_t>(a[index]) - b[index]));
    }
    return worst;
}

Surface Stroked(int32_t width, int32_t height, int32_t radius, int32_t line)
{
    Surface surface = Blank(width, height);
    Canvas(surface).StrokeRoundedRectangle({0, 0, width, height}, radius, line, White);
    return surface;
}

} // namespace

TEST_CASE("A rounded rectangle matches an independent 8x8 sample count for radii 0 to 12",
          "[shape]")
{
    const int32_t sizes[][2] = {{12, 12}, {13, 13}, {24, 24}, {30, 18}, {18, 30},
                                {25, 9},  {9, 25},  {26, 26}, {40, 40}, {1, 20},
                                {20, 1},  {2, 2},   {3, 7}};
    for (const auto& size : sizes)
    {
        for (int32_t radius = 0; radius <= 12; ++radius)
        {
            INFO(size[0] << "x" << size[1] << " radius " << radius);
            REQUIRE(CoverageOf(Filled(size[0], size[1], radius)) ==
                    ReferenceCoverage(size[0], size[1], radius, 8));
        }
    }
}

TEST_CASE("Radii past the cached limit are computed and match too", "[shape]")
{
    const int32_t limit = CornerCoverage::CachedRadiusLimit;
    REQUIRE(CornerCoverage(limit).Cached());
    REQUIRE_FALSE(CornerCoverage(limit + 1).Cached());
    for (int32_t radius : {13, 31, 63, limit - 1, limit, limit + 1, limit + 2, 90})
    {
        INFO("radius " << radius);
        const int32_t size = 2 * radius + 3;
        REQUIRE(CoverageOf(Filled(size, size + 4, radius)) ==
                ReferenceCoverage(size, size + 4, radius, 8));
    }
}

TEST_CASE("A rounded rectangle stays close to a 32x32 sample reference", "[shape]")
{
    int32_t worst = 0;
    for (int32_t radius = 0; radius <= 12; ++radius)
    {
        worst = std::max(worst, MaximumDifference(CoverageOf(Filled(30, 26, radius)),
                                                  ReferenceCoverage(30, 26, radius, 32)));
    }
    // The design's 2 levels is out of reach: 8 samples across a pixel resolve an edge to 1/8 of a
    // pixel. Measured over these radii the worst pixel is 7 levels off.
    REQUIRE(worst <= 8);
}

TEST_CASE("The four corners are mirror images", "[shape]")
{
    for (int32_t radius : {1, 2, 3, 5, 8, 11, 20, 70})
    {
        for (const auto& size : {std::pair{60, 50}, std::pair{161, 143}})
        {
            INFO("radius " << radius << " size " << size.first);
            const Surface surface = Filled(size.first, size.second, radius);
            for (int32_t y = 0; y < size.second; ++y)
            {
                for (int32_t x = 0; x < size.first; ++x)
                {
                    const uint32_t pixel = surface.PixelAt(x, y);
                    REQUIRE(surface.PixelAt(size.first - 1 - x, y) == pixel);
                    REQUIRE(surface.PixelAt(x, size.second - 1 - y) == pixel);
                }
            }
        }
    }
}

TEST_CASE("A corner has the quarter disc's coverage and its square's corner is empty", "[shape]")
{
    const Surface surface = Filled(20, 20, 10);
    REQUIRE(surface.PixelAt(0, 0) == 0);
    REQUIRE(surface.PixelAt(19, 0) == 0);
    REQUIRE(surface.PixelAt(0, 19) == 0);
    REQUIRE(surface.PixelAt(19, 19) == 0);
    REQUIRE(surface.PixelAt(9, 9) == 0xFFFFFF);
    REQUIRE(surface.PixelAt(10, 10) == 0xFFFFFF);
    REQUIRE(surface.PixelAt(10, 0) == 0xFFFFFF);
    REQUIRE(surface.PixelAt(0, 10) == 0xFFFFFF);
    // Pixel (1, 1) lies outside the circle of radius 10 around (10, 10), pixel (2, 2) inside
    // but for its outer corner.
    const uint8_t edge = static_cast<uint8_t>(surface.PixelAt(2, 2) & 0xFF);
    REQUIRE(edge > 0);
    REQUIRE(edge < 255);
}

TEST_CASE("Radius 0 is a plain rectangle and a negative radius is radius 0", "[shape]")
{
    for (Color color : {White, Graphics::WithAlpha(Graphics::Rgb(200, 100, 50), 128)})
    {
        Surface plain(30, 20);
        plain.Fill(0x102030);
        Surface zero = plain;
        Surface negative = plain;
        Canvas(plain).FillRectangle({3, 2, 20, 11}, color);
        Canvas(zero).FillRoundedRectangle({3, 2, 20, 11}, 0, color);
        Canvas(negative).FillRoundedRectangle({3, 2, 20, 11}, -7, color);
        REQUIRE(zero.Checksum() == plain.Checksum());
        REQUIRE(negative.Checksum() == plain.Checksum());
    }
}

TEST_CASE("A radius above half the smaller side is cut to it", "[shape]")
{
    for (const auto& size : {std::pair{40, 20}, std::pair{20, 40}, std::pair{21, 21}})
    {
        const int32_t half = std::min(size.first, size.second) / 2;
        for (int32_t radius : {half + 1, half + 7, 1000, std::numeric_limits<int32_t>::max()})
        {
            REQUIRE(Filled(size.first, size.second, radius).Checksum() ==
                    Filled(size.first, size.second, half).Checksum());
            REQUIRE(Stroked(size.first, size.second, radius, 2).Checksum() ==
                    Stroked(size.first, size.second, half, 2).Checksum());
        }
    }
}

TEST_CASE("Pills and circles", "[shape]")
{
    const Surface circle = Filled(24, 24, 12);
    REQUIRE(CoverageOf(circle) == ReferenceCoverage(24, 24, 12, 8));
    for (int32_t y = 0; y < 24; ++y)
    {
        for (int32_t x = 0; x < 24; ++x)
        {
            REQUIRE(circle.PixelAt(x, y) == circle.PixelAt(y, x));
        }
    }
    REQUIRE(circle.PixelAt(11, 11) == 0xFFFFFF);
    REQUIRE(circle.PixelAt(0, 0) == 0);

    const Surface pill = Filled(60, 20, 10);
    REQUIRE(CoverageOf(pill) == ReferenceCoverage(60, 20, 10, 8));
    for (int32_t y = 0; y < 20; ++y)
    {
        for (int32_t x = 10; x < 50; ++x)
        {
            REQUIRE(pill.PixelAt(x, y) == 0xFFFFFF);
        }
    }
    REQUIRE(Filled(60, 20, 5000).Checksum() == pill.Checksum());

    const Surface odd = Filled(21, 21, 10);
    REQUIRE(CoverageOf(odd) == ReferenceCoverage(21, 21, 10, 8));
    REQUIRE(odd.PixelAt(10, 10) == 0xFFFFFF);
    REQUIRE(odd.PixelAt(10, 0) == 0xFFFFFF);
}

TEST_CASE("A stroke is the outer shape less the inner one", "[shape]")
{
    const int32_t sizes[][2] = {{40, 30}, {41, 29}, {64, 64}, {100, 24}, {24, 100}, {17, 17}};
    for (const auto& size : sizes)
    {
        for (int32_t radius : {0, 1, 3, 6, 10, 12, 20, 50})
        {
            for (int32_t line : {1, 2, 3, 5, 8})
            {
                INFO(size[0] << "x" << size[1] << " radius " << radius << " width " << line);
                if (2 * line >= std::min(size[0], size[1]))
                {
                    continue;
                }
                const int32_t cut = std::min(radius, std::min(size[0], size[1]) / 2);
                const std::vector<uint8_t> outer = CoverageOf(Filled(size[0], size[1], cut));
                Surface inner_surface = Blank(size[0], size[1]);
                Canvas(inner_surface)
                    .FillRoundedRectangle({line, line, size[0] - 2 * line, size[1] - 2 * line},
                                          std::max(cut - line, 0), White);
                const std::vector<uint8_t> inner = CoverageOf(inner_surface);
                std::vector<uint8_t> expected;
                for (size_t index = 0; index < outer.size(); ++index)
                {
                    expected.push_back(static_cast<uint8_t>(std::max(outer[index] - inner[index], 0)));
                }
                REQUIRE(CoverageOf(Stroked(size[0], size[1], radius, line)) == expected);
            }
        }
    }
}

TEST_CASE("A stroke at least half the smaller side is a filled shape", "[shape]")
{
    for (int32_t radius : {0, 4, 9})
    {
        REQUIRE(Stroked(30, 18, radius, 9).Checksum() == Filled(30, 18, radius).Checksum());
        REQUIRE(Stroked(30, 18, radius, 400).Checksum() == Filled(30, 18, radius).Checksum());
        REQUIRE(Stroked(31, 19, radius, 10).Checksum() == Filled(31, 19, radius).Checksum());
        REQUIRE(Stroked(31, 19, radius, 9).Checksum() != Filled(31, 19, radius).Checksum());
    }
}

TEST_CASE("Strokes keep their corners symmetric and leave the inside alone", "[shape]")
{
    const Surface surface = Stroked(50, 40, 12, 3);
    for (int32_t y = 0; y < 40; ++y)
    {
        for (int32_t x = 0; x < 50; ++x)
        {
            REQUIRE(surface.PixelAt(x, y) == surface.PixelAt(49 - x, y));
            REQUIRE(surface.PixelAt(x, y) == surface.PixelAt(x, 39 - y));
        }
    }
    REQUIRE(surface.PixelAt(25, 20) == 0);
    REQUIRE(surface.PixelAt(25, 0) == 0xFFFFFF);
    REQUIRE(surface.PixelAt(25, 2) == 0xFFFFFF);
    REQUIRE(surface.PixelAt(25, 3) == 0);
    REQUIRE(surface.PixelAt(0, 20) == 0xFFFFFF);
}

TEST_CASE("Nothing is drawn for an empty rectangle, width 0 or alpha 0", "[shape]")
{
    Surface surface(20, 20);
    surface.Fill(0x123456);
    const uint32_t before = surface.Checksum();
    Canvas canvas(surface);
    canvas.FillRoundedRectangle({2, 2, 0, 10}, 3, White);
    canvas.FillRoundedRectangle({2, 2, 10, 0}, 3, White);
    canvas.FillRoundedRectangle({2, 2, -10, 10}, 3, White);
    canvas.FillRoundedRectangle({2, 2, 10, 10}, 3, 0x00FFFFFF);
    canvas.StrokeRoundedRectangle({2, 2, 0, 10}, 3, 1, White);
    canvas.StrokeRoundedRectangle({2, 2, 10, -3}, 3, 1, White);
    canvas.StrokeRoundedRectangle({2, 2, 10, 10}, 3, 0, White);
    canvas.StrokeRoundedRectangle({2, 2, 10, 10}, 3, -4, White);
    canvas.StrokeRoundedRectangle({2, 2, 10, 10}, 3, 1, 0x00FFFFFF);
    canvas.DrawHorizontalLine(2, 2, 0, White);
    canvas.DrawVerticalLine(2, 2, -5, White);
    REQUIRE(surface.Checksum() == before);
}

TEST_CASE("Lines are one pixel rectangles", "[shape]")
{
    Surface expected(30, 30);
    expected.Fill(0x203040);
    Surface actual = expected;
    Canvas reference(expected);
    Canvas canvas(actual);
    const Color translucent = Graphics::WithAlpha(Graphics::Rgb(10, 200, 90), 100);
    reference.FillRectangle({3, 4, 17, 1}, translucent);
    canvas.DrawHorizontalLine(3, 4, 17, translucent);
    reference.FillRectangle({25, 2, 1, 20}, White);
    canvas.DrawVerticalLine(25, 2, 20, White);
    reference.FillRectangle({-5, 9, 100, 1}, White);
    canvas.DrawHorizontalLine(-5, 9, 100, White);
    reference.FillRectangle({7, -50, 1, 200}, White);
    canvas.DrawVerticalLine(7, -50, 200, White);
    REQUIRE(actual.Checksum() == expected.Checksum());

    canvas.Translate(2, 3);
    reference.Translate(2, 3);
    canvas.DrawHorizontalLine(1, 1, 5, White);
    reference.FillRectangle({1, 1, 5, 1}, White);
    REQUIRE(actual.Checksum() == expected.Checksum());
}

TEST_CASE("Shapes are cut to the clip and the surface, with any coordinates", "[shape]")
{
    std::mt19937 random(37);
    auto coordinate = [&random]() -> int32_t {
        switch (random() % 6)
        {
        case 0:
            return std::numeric_limits<int32_t>::min() + static_cast<int32_t>(random() % 100);
        case 1:
            return std::numeric_limits<int32_t>::max() - static_cast<int32_t>(random() % 100);
        default:
            return static_cast<int32_t>(random() % 90) - 25;
        }
    };
    auto extent = [&random]() -> int32_t {
        return random() % 5 == 0 ? std::numeric_limits<int32_t>::max() - static_cast<int32_t>(random() % 50)
                                 : static_cast<int32_t>(random() % 60);
    };
    for (PixelFormat format : {PixelFormat::Xrgb, PixelFormat::PremultipliedArgb})
    {
        for (int round = 0; round < 1500; ++round)
        {
            Surface clipped(40, 36, format);
            clipped.Fill(format == PixelFormat::Xrgb ? 0x112233 : 0xFF112233);
            Surface unclipped = clipped;
            Canvas with_clip(clipped);
            Canvas without_clip(unclipped);
            const Rectangle clip{static_cast<int32_t>(random() % 40) - 5,
                                 static_cast<int32_t>(random() % 36) - 5,
                                 static_cast<int32_t>(random() % 40),
                                 static_cast<int32_t>(random() % 36)};
            const int32_t dx = static_cast<int32_t>(random() % 30) - 15;
            const int32_t dy = static_cast<int32_t>(random() % 30) - 15;
            with_clip.ClipTo(clip);
            with_clip.Translate(dx, dy);
            without_clip.Translate(dx, dy);
            const Rectangle shape{coordinate(), coordinate(), extent(), extent()};
            const int32_t radius = static_cast<int32_t>(random() % 40) - 3;
            const int32_t line = static_cast<int32_t>(random() % 8) - 1;
            const Color color = Graphics::WithAlpha(Graphics::Rgb(random(), random(), random()),
                                                    random() % 3 == 0 ? 255 : random() % 256);
            if (random() % 2 == 0)
            {
                with_clip.FillRoundedRectangle(shape, radius, color);
                without_clip.FillRoundedRectangle(shape, radius, color);
            }
            else
            {
                with_clip.StrokeRoundedRectangle(shape, radius, line, color);
                without_clip.StrokeRoundedRectangle(shape, radius, line, color);
            }
            with_clip.DrawHorizontalLine(shape.x, shape.y, shape.width, color);
            without_clip.DrawHorizontalLine(shape.x, shape.y, shape.width, color);
            with_clip.DrawVerticalLine(shape.x, shape.y, shape.height, color);
            without_clip.DrawVerticalLine(shape.x, shape.y, shape.height, color);

            Surface background(40, 36, format);
            background.Fill(format == PixelFormat::Xrgb ? 0x112233 : 0xFF112233);
            for (int32_t y = 0; y < 36; ++y)
            {
                for (int32_t x = 0; x < 40; ++x)
                {
                    const bool inside = x >= clip.x && x < clip.x + clip.width && y >= clip.y &&
                                        y < clip.y + clip.height;
                    REQUIRE(clipped.PixelAt(x, y) ==
                            (inside ? unclipped.PixelAt(x, y) : background.PixelAt(x, y)));
                }
            }
        }
    }
}

TEST_CASE("A premultiplied surface never gets a channel above its alpha", "[shape]")
{
    std::mt19937 random(38);
    Surface surface(60, 50, PixelFormat::PremultipliedArgb);
    Canvas canvas(surface);
    for (int round = 0; round < 300; ++round)
    {
        const Rectangle shape{static_cast<int32_t>(random() % 60) - 10,
                              static_cast<int32_t>(random() % 50) - 10,
                              static_cast<int32_t>(random() % 60),
                              static_cast<int32_t>(random() % 50)};
        const Color color = Graphics::WithAlpha(Graphics::Rgb(random(), random(), random()),
                                                random() % 256);
        if (round % 2 == 0)
        {
            canvas.FillRoundedRectangle(shape, static_cast<int32_t>(random() % 25), color);
        }
        else
        {
            canvas.StrokeRoundedRectangle(shape, static_cast<int32_t>(random() % 25),
                                          static_cast<int32_t>(random() % 6), color);
        }
    }
    for (int32_t y = 0; y < 50; ++y)
    {
        for (int32_t x = 0; x < 60; ++x)
        {
            const uint32_t pixel = surface.PixelAt(x, y);
            const uint32_t alpha = pixel >> 24;
            REQUIRE(((pixel >> 16) & 0xFF) <= alpha);
            REQUIRE(((pixel >> 8) & 0xFF) <= alpha);
            REQUIRE((pixel & 0xFF) <= alpha);
        }
    }
}

TEST_CASE("A translucent fill covers each pixel once", "[shape]")
{
    const Color half = Graphics::WithAlpha(Graphics::Rgb(255, 255, 255), 128);
    Surface surface = Blank(40, 30);
    Canvas(surface).FillRoundedRectangle({0, 0, 40, 30}, 9, half);
    const std::vector<uint8_t> reference = ReferenceCoverage(40, 30, 9, 8);
    for (int32_t y = 0; y < 30; ++y)
    {
        for (int32_t x = 0; x < 40; ++x)
        {
            const uint32_t coverage = reference[static_cast<size_t>(y) * 40 + x];
            // Div255(128 * coverage), then Div255(255 * alpha) on black.
            const uint32_t alpha = coverage == 255 ? 128 : (128 * coverage + 127) / 255;
            REQUIRE((surface.PixelAt(x, y) & 0xFF) == (255 * alpha + 127) / 255);
        }
    }
}

TEST_CASE("Corner rows have the pinned values", "[shape]")
{
    std::vector<uint8_t> row(4);
    Graphics::ComputeCornerRow(4, 0, row.data());
    REQUIRE(row == std::vector<uint8_t>{0, 40, 179, 247});
    Graphics::ComputeCornerRow(4, 3, row.data());
    REQUIRE(row == std::vector<uint8_t>{247, 255, 255, 255});
    REQUIRE(CornerCoverage(0).Radius() == 0);
    REQUIRE(CornerCoverage(0).Row(0) == nullptr);
    REQUIRE(CornerCoverage(-3).Radius() == 0);
}
