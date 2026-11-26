#include <catch2/catch_test_macros.hpp>

#include "user/graphics/canvas.h"
#include "user/graphics/color.h"
#include "user/graphics/mask.h"
#include "user/graphics/surface.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <random>
#include <vector>

namespace Graphics = FeatherOS::Graphics;
using Graphics::Canvas;
using Graphics::Color;
using Graphics::Mask;
using Graphics::PixelFormat;
using Graphics::Rectangle;
using Graphics::Surface;

namespace
{

constexpr int32_t Min32 = std::numeric_limits<int32_t>::min();
constexpr int32_t Max32 = std::numeric_limits<int32_t>::max();

constexpr uint32_t PinnedXrgb = 0x6e47e444;
constexpr uint32_t PinnedPremultiplied = 0xa415223c;

uint32_t Round255(uint32_t n)
{
    return (2 * n + 255) / 510;
}

uint32_t Channel(uint32_t pixel, int shift)
{
    return (pixel >> shift) & 0xFF;
}

uint32_t Pack(uint32_t a, uint32_t r, uint32_t g, uint32_t b)
{
    return a << 24 | r << 16 | g << 8 | b;
}

uint32_t ReferenceMix(uint32_t destination, uint32_t color, uint32_t alpha)
{
    uint32_t result = 0;
    for (int shift = 16; shift >= 0; shift -= 8)
    {
        result |= Round255(Channel(color, shift) * alpha + Channel(destination, shift) * (255 - alpha))
                  << shift;
    }
    return result;
}

uint32_t ReferenceOver(uint32_t destination, uint32_t source, bool with_alpha)
{
    const uint32_t alpha = source >> 24;
    uint32_t result = 0;
    for (int shift = with_alpha ? 24 : 16; shift >= 0; shift -= 8)
    {
        const uint32_t value =
            Channel(source, shift) + Round255(Channel(destination, shift) * (255 - alpha));
        result |= (value > 255 ? 255 : value) << shift;
    }
    return result;
}

uint32_t ReferenceScale(uint32_t pixel, uint32_t factor)
{
    return Pack(Round255(Channel(pixel, 24) * factor), Round255(Channel(pixel, 16) * factor),
                Round255(Channel(pixel, 8) * factor), Round255(Channel(pixel, 0) * factor));
}

uint32_t ReferencePremultiply(Color color, uint32_t alpha)
{
    return Pack(alpha, Round255(Channel(color, 16) * alpha), Round255(Channel(color, 8) * alpha),
                Round255(Channel(color, 0) * alpha));
}

uint32_t ReferenceColor(uint32_t destination, Color color, uint32_t alpha, bool premultiplied)
{
    if (!premultiplied)
    {
        return ReferenceMix(destination, color, alpha);
    }
    return ReferenceOver(destination, ReferencePremultiply(color, alpha), true);
}

uint32_t ReferenceSurfacePixel(uint32_t destination, uint32_t source, bool source_premultiplied,
                               bool premultiplied, uint32_t opacity)
{
    if (!source_premultiplied && !premultiplied)
    {
        return ReferenceMix(destination, source, opacity);
    }
    const uint32_t pixel = source_premultiplied ? source : (source & 0xFFFFFF) | 0xFF000000U;
    return ReferenceOver(destination, ReferenceScale(pixel, opacity), premultiplied);
}

// Everything is recomputed per pixel in 64-bit, with no clamping of the caller's numbers.
class Model
{
public:
    Model(int32_t width, int32_t height, bool premultiplied, std::vector<uint32_t> pixels)
        : m_width(width), m_height(height), m_premultiplied(premultiplied),
          m_pixels(std::move(pixels)), m_state{0, 0, 0, 0, width, height}
    {
    }

    const std::vector<uint32_t>& Pixels() const
    {
        return m_pixels;
    }

    bool Save()
    {
        if (m_saved.size() == Canvas::MaximumDepth)
        {
            ++m_refused;
            return false;
        }
        m_saved.push_back(m_state);
        return true;
    }

    void Restore()
    {
        if (m_refused > 0)
        {
            --m_refused;
        }
        else if (!m_saved.empty())
        {
            m_state = m_saved.back();
            m_saved.pop_back();
        }
    }

    void ClipTo(const Rectangle& r)
    {
        const int64_t x = static_cast<int64_t>(r.x) + m_state.ox;
        const int64_t y = static_cast<int64_t>(r.y) + m_state.oy;
        if (r.width <= 0 || r.height <= 0)
        {
            m_state.left = m_state.right = m_state.top = m_state.bottom = 0;
            return;
        }
        m_state.left = std::max(m_state.left, x);
        m_state.top = std::max(m_state.top, y);
        m_state.right = std::min(m_state.right, x + r.width);
        m_state.bottom = std::min(m_state.bottom, y + r.height);
    }

    void Translate(int32_t dx, int32_t dy)
    {
        m_state.ox += dx;
        m_state.oy += dy;
    }

    bool ClipEmpty() const
    {
        return m_state.left >= m_state.right || m_state.top >= m_state.bottom;
    }

    Rectangle Clip() const
    {
        return {static_cast<int32_t>(m_state.left), static_cast<int32_t>(m_state.top),
                static_cast<int32_t>(m_state.right - m_state.left),
                static_cast<int32_t>(m_state.bottom - m_state.top)};
    }

    void Fill(const Rectangle& r, Color color)
    {
        const uint32_t alpha = color >> 24;
        for (int32_t y = 0; y < m_height; ++y)
        {
            for (int32_t x = 0; x < m_width; ++x)
            {
                const int64_t dx = x - (static_cast<int64_t>(r.x) + m_state.ox);
                const int64_t dy = y - (static_cast<int64_t>(r.y) + m_state.oy);
                if (alpha != 0 && Inside(x, y) && dx >= 0 && dx < r.width && dy >= 0 &&
                    dy < r.height)
                {
                    Pixel(x, y) = ReferenceColor(Pixel(x, y), color, alpha, m_premultiplied);
                }
            }
        }
    }

    void Draw(const Surface& source, int32_t sx, int32_t sy, int32_t width, int32_t height,
              int32_t px, int32_t py, uint32_t opacity)
    {
        const bool source_premultiplied = source.Format() == PixelFormat::PremultipliedArgb;
        for (int32_t y = 0; y < m_height; ++y)
        {
            for (int32_t x = 0; x < m_width; ++x)
            {
                const int64_t u = x - (static_cast<int64_t>(px) + m_state.ox) + sx;
                const int64_t v = y - (static_cast<int64_t>(py) + m_state.oy) + sy;
                if (opacity != 0 && Inside(x, y) && u >= sx && u < static_cast<int64_t>(sx) + width &&
                    v >= sy && v < static_cast<int64_t>(sy) + height && u >= 0 &&
                    u < source.Width() && v >= 0 && v < source.Height())
                {
                    Pixel(x, y) = ReferenceSurfacePixel(
                        Pixel(x, y), source.PixelAt(static_cast<int32_t>(u), static_cast<int32_t>(v)),
                        source_premultiplied, m_premultiplied, opacity);
                }
            }
        }
    }

    void DrawMask(const Mask& mask, int32_t px, int32_t py, Color color)
    {
        for (int32_t y = 0; y < m_height; ++y)
        {
            for (int32_t x = 0; x < m_width; ++x)
            {
                const int64_t u = x - (static_cast<int64_t>(px) + m_state.ox);
                const int64_t v = y - (static_cast<int64_t>(py) + m_state.oy);
                if (!Inside(x, y) || u < 0 || u >= mask.Width() || v < 0 || v >= mask.Height())
                {
                    continue;
                }
                const uint32_t coverage =
                    mask.CoverageAt(static_cast<int32_t>(u), static_cast<int32_t>(v));
                const uint32_t alpha = Round255((color >> 24) * coverage);
                if (alpha != 0)
                {
                    Pixel(x, y) = ReferenceColor(Pixel(x, y), color, alpha, m_premultiplied);
                }
            }
        }
    }

private:
    struct State
    {
        int64_t ox;
        int64_t oy;
        int64_t left;
        int64_t top;
        int64_t right;
        int64_t bottom;
    };

    bool Inside(int32_t x, int32_t y) const
    {
        return x >= m_state.left && x < m_state.right && y >= m_state.top && y < m_state.bottom;
    }

    uint32_t& Pixel(int32_t x, int32_t y)
    {
        return m_pixels[static_cast<size_t>(y) * static_cast<size_t>(m_width) +
                        static_cast<size_t>(x)];
    }

    int32_t m_width;
    int32_t m_height;
    bool m_premultiplied;
    std::vector<uint32_t> m_pixels;
    State m_state;
    std::vector<State> m_saved;
    size_t m_refused = 0;
};

uint32_t RandomPixel(std::mt19937& random, bool premultiplied)
{
    if (!premultiplied)
    {
        return random() & 0xFFFFFF;
    }
    const uint32_t shape = random() % 6;
    const uint32_t alpha = shape == 0 ? 0 : shape == 1 ? 255 : random() % 256;
    auto channel = [&]() { return random() % (alpha + 1); };
    return Pack(alpha, channel(), channel(), channel());
}

void Randomize(Surface& surface, std::mt19937& random)
{
    const bool premultiplied = surface.Format() == PixelFormat::PremultipliedArgb;
    for (int32_t y = 0; y < surface.Height(); ++y)
    {
        for (int32_t x = 0; x < surface.Width(); ++x)
        {
            surface.Pixels()[y * surface.Width() + x] = RandomPixel(random, premultiplied);
        }
    }
}

std::vector<uint32_t> PixelsOf(const Surface& surface)
{
    return {surface.Pixels(), surface.Pixels() + static_cast<size_t>(surface.Width()) *
                                                     static_cast<size_t>(surface.Height())};
}

int32_t RandomCoordinate(std::mt19937& random)
{
    switch (random() % 10)
    {
    case 0:
        return random() % 2 == 0 ? Min32 : Max32;
    case 1:
        return static_cast<int32_t>(random() % 2000001) - 1000000;
    case 2:
        return static_cast<int32_t>(random() % 2) == 0 ? -16384 - static_cast<int32_t>(random() % 3)
                                                       : 16384 + static_cast<int32_t>(random() % 3);
    default:
        return static_cast<int32_t>(random() % 110) - 45;
    }
}

int32_t RandomExtent(std::mt19937& random)
{
    switch (random() % 10)
    {
    case 0:
        return 0;
    case 1:
        return Max32;
    case 2:
        return 1 << 30;
    case 3:
        return static_cast<int32_t>(random() % 3) - 2 - static_cast<int32_t>(random() % 40);
    default:
        return static_cast<int32_t>(random() % 60);
    }
}

Color RandomColor(std::mt19937& random)
{
    const uint32_t shape = random() % 6;
    const uint32_t alpha = shape == 0 ? 0 : shape == 1 ? 255 : random() % 256;
    return (alpha << 24) | (random() & 0xFFFFFF);
}

Mask RandomMask(std::mt19937& random)
{
    Mask mask(1 + static_cast<int32_t>(random() % 20), 1 + static_cast<int32_t>(random() % 20));
    for (int32_t y = 0; y < mask.Height(); ++y)
    {
        for (int32_t x = 0; x < mask.Width(); ++x)
        {
            const uint32_t shape = random() % 5;
            mask.SetCoverage(x, y,
                             static_cast<uint8_t>(shape == 0   ? 0
                                                  : shape == 1 ? 255
                                                               : random() % 256));
        }
    }
    return mask;
}

Surface RandomSource(std::mt19937& random)
{
    Surface source(1 + static_cast<int32_t>(random() % 20), 1 + static_cast<int32_t>(random() % 20),
                   random() % 2 == 0 ? PixelFormat::Xrgb : PixelFormat::PremultipliedArgb);
    Randomize(source, random);
    return source;
}

void CheckClip(const Canvas& canvas, const Model& model)
{
    const Rectangle clip = canvas.Clip();
    if (model.ClipEmpty())
    {
        REQUIRE((clip.width <= 0 || clip.height <= 0));
        return;
    }
    const Rectangle expected = model.Clip();
    REQUIRE(clip.x == expected.x);
    REQUIRE(clip.y == expected.y);
    REQUIRE(clip.width == expected.width);
    REQUIRE(clip.height == expected.height);
}

void RunRandomCalls(PixelFormat format, uint32_t seed)
{
    std::mt19937 random(seed);
    const bool premultiplied = format == PixelFormat::PremultipliedArgb;
    std::unique_ptr<Surface> surface;
    std::unique_ptr<Canvas> canvas;
    std::unique_ptr<Model> model;
    for (int call = 0; call < 5000; ++call)
    {
        if (call % 25 == 0)
        {
            surface = std::make_unique<Surface>(1 + static_cast<int32_t>(random() % 40),
                                                1 + static_cast<int32_t>(random() % 30), format);
            Randomize(*surface, random);
            canvas = std::make_unique<Canvas>(*surface);
            model = std::make_unique<Model>(surface->Width(), surface->Height(), premultiplied,
                                            PixelsOf(*surface));
        }
        const uint32_t kind = random() % 100;
        const Rectangle rectangle{RandomCoordinate(random), RandomCoordinate(random),
                                  RandomExtent(random), RandomExtent(random)};
        const int32_t x = RandomCoordinate(random);
        const int32_t y = RandomCoordinate(random);
        INFO("call " << call << " kind " << kind);
        if (kind < 25)
        {
            const Color color = RandomColor(random);
            canvas->FillRectangle(rectangle, color);
            model->Fill(rectangle, color);
        }
        else if (kind < 40)
        {
            const Surface source = RandomSource(random);
            canvas->DrawSurfacePart(source, rectangle.x, rectangle.y, rectangle.width,
                                    rectangle.height, x, y);
            model->Draw(source, rectangle.x, rectangle.y, rectangle.width, rectangle.height, x, y,
                        255);
        }
        else if (kind < 50)
        {
            const Surface source = RandomSource(random);
            canvas->DrawSurface(source, x, y);
            model->Draw(source, 0, 0, source.Width(), source.Height(), x, y, 255);
        }
        else if (kind < 60)
        {
            const Surface source = RandomSource(random);
            const auto opacity = static_cast<uint8_t>(random() % 4 == 0 ? 255 : random() % 256);
            canvas->DrawSurfaceFaded(source, x, y, opacity);
            model->Draw(source, 0, 0, source.Width(), source.Height(), x, y, opacity);
        }
        else if (kind < 75)
        {
            const Mask mask = RandomMask(random);
            const Color color = RandomColor(random);
            canvas->DrawMask(mask, x, y, color);
            model->DrawMask(mask, x, y, color);
        }
        else if (kind < 85)
        {
            canvas->ClipTo(rectangle);
            model->ClipTo(rectangle);
        }
        else if (kind < 92)
        {
            canvas->Translate(x, y);
            model->Translate(x, y);
        }
        else if (kind < 96)
        {
            REQUIRE(canvas->Save() == model->Save());
        }
        else
        {
            canvas->Restore();
            model->Restore();
        }
        CheckClip(*canvas, *model);
        REQUIRE(PixelsOf(*surface) == model->Pixels());
    }
}

} // namespace

TEST_CASE("Every operation matches a per-pixel reference on an XRGB surface", "[canvas]")
{
    RunRandomCalls(PixelFormat::Xrgb, 51);
}

TEST_CASE("Every operation matches a per-pixel reference on a premultiplied surface", "[canvas]")
{
    RunRandomCalls(PixelFormat::PremultipliedArgb, 52);
}

namespace
{

struct StateView
{
    int64_t origin_x;
    int64_t origin_y;
    Rectangle clip;

    bool operator==(const StateView& other) const
    {
        return origin_x == other.origin_x && origin_y == other.origin_y &&
               clip.x == other.clip.x && clip.y == other.clip.y &&
               clip.width == other.clip.width && clip.height == other.clip.height;
    }
};

StateView View(const Canvas& canvas)
{
    return {canvas.OriginX(), canvas.OriginY(), canvas.Clip()};
}

} // namespace

TEST_CASE("Restore returns the origin and the clip to what Save saw", "[canvas]")
{
    Surface surface(64, 48);
    Canvas canvas(surface);
    const StateView initial = View(canvas);
    REQUIRE(initial.clip.width == 64);
    REQUIRE(initial.clip.height == 48);

    canvas.Translate(5, 6);
    canvas.ClipTo({0, 0, 30, 20});
    const StateView first = View(canvas);
    REQUIRE(first.clip.x == 5);
    REQUIRE(first.clip.width == 30);

    REQUIRE(canvas.Save());
    canvas.Translate(-100, 7);
    canvas.ClipTo({-100, 0, 110, 5});
    const StateView second = View(canvas);
    REQUIRE_FALSE(second == first);
    REQUIRE(canvas.Save());
    canvas.ClipTo({1000, 1000, 5, 5});
    REQUIRE(canvas.Clip().width == 0);
    canvas.Restore();
    REQUIRE(View(canvas) == second);
    canvas.Restore();
    REQUIRE(View(canvas) == first);
    canvas.Restore();
    REQUIRE(View(canvas) == first);
}

TEST_CASE("ClipTo intersects and is relative to the origin", "[canvas]")
{
    Surface surface(20, 20);
    Canvas canvas(surface);
    canvas.Translate(4, 4);
    canvas.ClipTo({2, 2, 100, 100});
    REQUIRE(canvas.Clip().x == 6);
    REQUIRE(canvas.Clip().y == 6);
    REQUIRE(canvas.Clip().width == 14);
    canvas.ClipTo({-50, -50, 60, 60});
    REQUIRE(canvas.Clip().x == 6);
    REQUIRE(canvas.Clip().width == 8);
    canvas.ClipTo({0, 0, 0, 10});
    REQUIRE(canvas.Clip().width == 0);
    canvas.FillRectangle({-100, -100, 1000, 1000}, 0xFFFFFFFF);
    for (int32_t index = 0; index < 400; ++index)
    {
        REQUIRE(surface.Pixels()[index] == 0);
    }
}

TEST_CASE("Save beyond depth 16 returns false and its Restore does nothing", "[canvas]")
{
    Surface surface(64, 64);
    Canvas canvas(surface);
    std::vector<StateView> views;
    for (int32_t depth = 0; depth < 16; ++depth)
    {
        views.push_back(View(canvas));
        REQUIRE(canvas.Save());
        canvas.Translate(1, 2);
        canvas.ClipTo({0, 0, 60 - depth, 60 - depth});
    }
    const StateView deepest = View(canvas);
    REQUIRE_FALSE(canvas.Save());
    canvas.Translate(3, 3);
    canvas.Restore();
    REQUIRE(canvas.OriginX() == deepest.origin_x + 3);
    REQUIRE(canvas.Clip().width == deepest.clip.width);
    for (int32_t depth = 15; depth >= 0; --depth)
    {
        canvas.Restore();
        REQUIRE(View(canvas) == views[static_cast<size_t>(depth)]);
    }
    canvas.Restore();
    REQUIRE(View(canvas) == views[0]);
    REQUIRE(canvas.Save());
}

TEST_CASE("An opaque fill is byte-equal to Surface::FillRectangle", "[canvas]")
{
    std::mt19937 random(61);
    Surface expected(37, 23);
    Surface actual(37, 23);
    Randomize(expected, random);
    actual = expected;
    Canvas canvas(actual);
    for (int round = 0; round < 500; ++round)
    {
        const Rectangle rectangle{RandomCoordinate(random), RandomCoordinate(random),
                                  RandomExtent(random), RandomExtent(random)};
        const Color color = 0xFF000000U | (random() & 0xFFFFFF);
        expected.FillRectangle(rectangle.x, rectangle.y, rectangle.width, rectangle.height, color);
        canvas.FillRectangle(rectangle, color);
        REQUIRE(actual.Checksum() == expected.Checksum());
    }
    REQUIRE(PixelsOf(actual) == PixelsOf(expected));
}

TEST_CASE("A copy of an XRGB surface equals Surface::Blit", "[canvas]")
{
    std::mt19937 random(62);
    Surface expected(30, 30);
    Randomize(expected, random);
    Surface actual = expected;
    for (int round = 0; round < 200; ++round)
    {
        Surface source(1 + static_cast<int32_t>(random() % 20),
                       1 + static_cast<int32_t>(random() % 20));
        Randomize(source, random);
        const int32_t x = static_cast<int32_t>(random() % 60) - 25;
        const int32_t y = static_cast<int32_t>(random() % 60) - 25;
        expected.Blit(&source, x, y);
        Canvas(actual).DrawSurface(source, x, y);
        REQUIRE(PixelsOf(actual) == PixelsOf(expected));
    }
}

TEST_CASE("A colour with alpha 0 draws nothing and a mask of zeros draws nothing", "[canvas]")
{
    Surface surface(8, 8);
    std::mt19937 random(63);
    Randomize(surface, random);
    const std::vector<uint32_t> before = PixelsOf(surface);
    Canvas canvas(surface);
    canvas.FillRectangle({0, 0, 8, 8}, 0x00FFFFFF);
    Mask mask(8, 8);
    canvas.DrawMask(mask, 0, 0, 0xFFFFFFFF);
    Mask full(8, 8);
    for (int32_t index = 0; index < 64; ++index)
    {
        full.Data()[index] = 255;
    }
    canvas.DrawMask(full, 0, 0, 0x00FFFFFF);
    REQUIRE(PixelsOf(surface) == before);
    canvas.DrawMask(full, 0, 0, 0xFF123456);
    for (const uint32_t pixel : PixelsOf(surface))
    {
        REQUIRE(pixel == 0x123456);
    }
}

TEST_CASE("An invalid surface, source or mask is ignored", "[canvas]")
{
    Surface invalid(0, 0);
    Canvas on_invalid(invalid);
    Surface valid(4, 4);
    on_invalid.FillRectangle({0, 0, 4, 4}, 0xFFFFFFFF);
    on_invalid.DrawSurface(valid, 0, 0);
    on_invalid.ClipTo({0, 0, 4, 4});
    REQUIRE(on_invalid.Clip().width == 0);

    Canvas canvas(valid);
    canvas.DrawSurface(invalid, 0, 0);
    canvas.DrawSurface(valid, 1, 1);
    canvas.DrawMask(Mask(0, 5), 0, 0, 0xFFFFFFFF);
    REQUIRE(valid.Checksum() == Surface(4, 4).Checksum());
}

TEST_CASE("A surface keeps its format through copies and moves", "[canvas]")
{
    Surface plain(4, 4);
    REQUIRE(plain.Format() == PixelFormat::Xrgb);
    Surface premultiplied(4, 4, PixelFormat::PremultipliedArgb);
    REQUIRE(premultiplied.Format() == PixelFormat::PremultipliedArgb);
    Surface copy = premultiplied;
    REQUIRE(copy.Format() == PixelFormat::PremultipliedArgb);
    plain = premultiplied;
    REQUIRE(plain.Format() == PixelFormat::PremultipliedArgb);
    Surface moved = std::move(copy);
    REQUIRE(moved.Format() == PixelFormat::PremultipliedArgb);
    plain = Surface(2, 2);
    REQUIRE(plain.Format() == PixelFormat::Xrgb);
}

TEST_CASE("Premultiplied surfaces fill and checksum with their alpha", "[canvas]")
{
    Surface surface(2, 2, PixelFormat::PremultipliedArgb);
    surface.Fill(0x80402010);
    REQUIRE(surface.PixelAt(1, 1) == 0x80402010);
    Surface other(2, 2, PixelFormat::PremultipliedArgb);
    other.Fill(0x00402010);
    REQUIRE(surface.Checksum() != other.Checksum());
}

TEST_CASE("A half-transparent fill and mask blend as the rules say", "[canvas]")
{
    Surface xrgb(2, 1);
    xrgb.Fill(0x00FF00);
    Canvas(xrgb).FillRectangle({0, 0, 1, 1}, Graphics::WithAlpha(Graphics::Rgb(255, 0, 0), 128));
    // Div255(255 * 128 + 0 * 127), Div255(0 * 128 + 255 * 127), 0
    REQUIRE(xrgb.PixelAt(0, 0) == (0x80U << 16 | 0x7FU << 8));
    REQUIRE(xrgb.PixelAt(1, 0) == 0x00FF00);

    Surface premultiplied(1, 1, PixelFormat::PremultipliedArgb);
    Canvas on_premultiplied(premultiplied);
    on_premultiplied.FillRectangle({0, 0, 1, 1}, Graphics::WithAlpha(Graphics::Rgb(255, 0, 0), 128));
    REQUIRE(premultiplied.PixelAt(0, 0) == 0x80800000);
    on_premultiplied.FillRectangle({0, 0, 1, 1}, Graphics::WithAlpha(Graphics::Rgb(255, 0, 0), 128));
    REQUIRE(premultiplied.PixelAt(0, 0) == 0xC0C00000);

    Surface masked(1, 1);
    Mask mask(1, 1);
    mask.SetCoverage(0, 0, 128);
    Canvas(masked).DrawMask(mask, 0, 0, Graphics::Rgb(255, 255, 255));
    REQUIRE(masked.PixelAt(0, 0) == 0x808080);
}

TEST_CASE("Fixed scenes have pinned checksums", "[canvas]")
{
    for (const PixelFormat format : {PixelFormat::Xrgb, PixelFormat::PremultipliedArgb})
    {
        Surface surface(48, 32, format);
        Surface sprite(10, 10, PixelFormat::PremultipliedArgb);
        Canvas(sprite).FillRectangle({0, 0, 10, 10}, 0x80FF8000);
        Canvas(sprite).FillRectangle({3, 3, 4, 4}, 0xFF0000FF);
        Surface plain(8, 8);
        plain.Fill(0x336699);
        Mask mask(12, 12);
        for (int32_t y = 0; y < 12; ++y)
        {
            for (int32_t x = 0; x < 12; ++x)
            {
                mask.SetCoverage(x, y, static_cast<uint8_t>((x * 23 + y * 11) % 256));
            }
        }
        Canvas canvas(surface);
        canvas.FillRectangle({0, 0, 48, 32}, 0xFF203040);
        canvas.FillRectangle({4, 4, 30, 20}, 0xC0E0A060);
        canvas.DrawSurface(sprite, 20, 10);
        canvas.DrawSurface(plain, 40, 28);
        canvas.DrawSurfaceFaded(plain, 2, 2, 100);
        canvas.Save();
        canvas.Translate(10, 12);
        canvas.ClipTo({0, 0, 20, 10});
        canvas.DrawMask(mask, -2, 0, 0xFFFFFFFF);
        canvas.DrawSurfaceFaded(sprite, 5, 5, 200);
        canvas.Restore();
        canvas.DrawSurfacePart(sprite, 2, 2, 5, 5, 0, 25);
        const uint32_t expected =
            format == PixelFormat::Xrgb ? PinnedXrgb : PinnedPremultiplied;
        REQUIRE(surface.Checksum() == expected);
    }
}

namespace
{

Surface Checkerboard(int32_t width, int32_t height)
{
    Surface surface(width, height);
    for (int32_t y = 0; y < height; ++y)
    {
        for (int32_t x = 0; x < width; ++x)
        {
            surface.Pixels()[y * width + x] = (x + y) % 2 == 0 ? 0xFFFFFF : 0x000000;
        }
    }
    return surface;
}

uint32_t BlockMean(const Surface& source, int32_t x, int32_t y, int32_t width, int32_t height)
{
    uint64_t sums[3] = {0, 0, 0};
    for (int32_t row = y; row < y + height; ++row)
    {
        for (int32_t column = x; column < x + width; ++column)
        {
            const uint32_t pixel = source.PixelAt(column, row);
            for (int channel = 0; channel < 3; ++channel)
            {
                sums[channel] += (pixel >> (8 * channel)) & 0xFF;
            }
        }
    }
    const uint64_t count = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    uint32_t result = 0;
    for (int channel = 0; channel < 3; ++channel)
    {
        result |= static_cast<uint32_t>((sums[channel] + count / 2) / count) << (8 * channel);
    }
    return result;
}

} // namespace

TEST_CASE("Halving a checkerboard gives the mean", "[canvas]")
{
    const Surface source = Checkerboard(16, 16);
    Surface target(8, 8);
    Canvas(target).DrawSurfaceScaled(source, {0, 0, 8, 8});
    for (int32_t y = 0; y < 8; ++y)
    {
        for (int32_t x = 0; x < 8; ++x)
        {
            REQUIRE(target.PixelAt(x, y) == 0x808080);
        }
    }
    // A quarter size takes the mean of 4 x 4 blocks, and a third of 3 x 3 blocks of 9 pixels.
    Surface quarter(4, 4);
    Canvas(quarter).DrawSurfaceScaled(source, {0, 0, 4, 4});
    CHECK(quarter.PixelAt(2, 2) == 0x808080);
    const Surface odd = Checkerboard(9, 9);
    Surface third(3, 3);
    Canvas(third).DrawSurfaceScaled(odd, {0, 0, 3, 3});
    CHECK(third.PixelAt(0, 0) == 0x8E8E8E);
}

TEST_CASE("Shrinking by a whole factor is the block mean and enlarging repeats pixels",
          "[canvas]")
{
    std::mt19937 random(61);
    Surface source(12, 8);
    for (int32_t i = 0; i < 12 * 8; ++i)
    {
        source.Pixels()[i] = static_cast<uint32_t>(random()) & 0xFFFFFF;
    }
    Surface half(6, 4);
    Canvas(half).DrawSurfaceScaled(source, {0, 0, 6, 4});
    Surface mixed(3, 8);
    Canvas(mixed).DrawSurfaceScaled(source, {0, 0, 3, 8});
    for (int32_t y = 0; y < 4; ++y)
    {
        for (int32_t x = 0; x < 6; ++x)
        {
            REQUIRE(half.PixelAt(x, y) == BlockMean(source, 2 * x, 2 * y, 2, 2));
        }
    }
    for (int32_t y = 0; y < 8; ++y)
    {
        for (int32_t x = 0; x < 3; ++x)
        {
            REQUIRE(mixed.PixelAt(x, y) == BlockMean(source, 4 * x, y, 4, 1));
        }
    }
    Surface big(36, 24);
    Canvas(big).DrawSurfaceScaled(source, {0, 0, 36, 24});
    for (int32_t y = 0; y < 24; ++y)
    {
        for (int32_t x = 0; x < 36; ++x)
        {
            REQUIRE(big.PixelAt(x, y) == source.PixelAt(x / 3, y / 3));
        }
    }
    Surface same(12, 8);
    Canvas(same).DrawSurfaceScaled(source, {0, 0, 12, 8});
    CHECK(PixelsOf(same) == PixelsOf(source));
}

TEST_CASE("A scaled surface composites by its format and obeys the clip", "[canvas]")
{
    Surface source(4, 4, PixelFormat::PremultipliedArgb);
    Canvas(source).FillRectangle({0, 0, 4, 4}, 0x80FF0000);
    Surface target(8, 8);
    target.Fill(0x000000);
    Canvas canvas(target);
    canvas.ClipTo({2, 2, 3, 3});
    canvas.DrawSurfaceScaled(source, {0, 0, 8, 8});
    const uint32_t expected = ReferenceOver(0x000000, source.PixelAt(0, 0), false);
    for (int32_t y = 0; y < 8; ++y)
    {
        for (int32_t x = 0; x < 8; ++x)
        {
            const bool inside = x >= 2 && x < 5 && y >= 2 && y < 5;
            REQUIRE(target.PixelAt(x, y) == (inside ? expected : 0x000000));
        }
    }

    Surface premultiplied(8, 8, PixelFormat::PremultipliedArgb);
    Canvas(premultiplied).DrawSurfaceScaled(source, {0, 0, 8, 8});
    CHECK(premultiplied.PixelAt(3, 3) == source.PixelAt(0, 0));
}

TEST_CASE("Scaling ignores an empty destination, an invalid source and the target itself",
          "[canvas]")
{
    Surface source = Checkerboard(4, 4);
    Surface target(8, 8);
    target.Fill(0x102030);
    const std::vector<uint32_t> before = PixelsOf(target);
    Canvas canvas(target);
    canvas.DrawSurfaceScaled(source, {0, 0, 0, 4});
    canvas.DrawSurfaceScaled(source, {0, 0, 4, -1});
    canvas.DrawSurfaceScaled(Surface(0, 0), {0, 0, 4, 4});
    canvas.DrawSurfaceScaled(target, {0, 0, 4, 4});
    canvas.DrawSurfaceScaled(source, {100, 100, 4, 4});
    CHECK(PixelsOf(target) == before);
}

TEST_CASE("Scaling never writes outside the clip for extreme destinations", "[canvas]")
{
    std::mt19937 random(62);
    Surface source = Checkerboard(7, 5);
    for (const PixelFormat format : {PixelFormat::Xrgb, PixelFormat::PremultipliedArgb})
    {
        for (int round = 0; round < 300; ++round)
        {
            Surface target(30, 24, format);
            Canvas canvas(target);
            canvas.FillRectangle({0, 0, 30, 24}, 0xFF446688);
            const std::vector<uint32_t> before = PixelsOf(target);
            auto pick = [&](int32_t extent) {
                const uint32_t kind = random() % 8;
                if (kind == 0)
                {
                    return Min32 + static_cast<int32_t>(random() % 5);
                }
                if (kind == 1)
                {
                    return Max32 - static_cast<int32_t>(random() % 5);
                }
                return static_cast<int32_t>(random() % (2 * extent + 20)) - extent - 10;
            };
            canvas.ClipTo({pick(30), pick(24), static_cast<int32_t>(random() % 40),
                           static_cast<int32_t>(random() % 30)});
            const Rectangle visible = canvas.Clip();
            const int32_t width = random() % 4 == 0 ? Max32 - static_cast<int32_t>(random() % 3)
                                                   : static_cast<int32_t>(random() % 60);
            const int32_t height = static_cast<int32_t>(random() % 50);
            canvas.DrawSurfaceScaled(source, {pick(30), pick(24), width, height});
            for (int32_t y = 0; y < 24; ++y)
            {
                for (int32_t x = 0; x < 30; ++x)
                {
                    const bool inside = x >= visible.x && x < visible.x + visible.width &&
                                        y >= visible.y && y < visible.y + visible.height;
                    if (!inside)
                    {
                        REQUIRE(target.PixelAt(x, y) == before[static_cast<size_t>(y) * 30 + x]);
                    }
                }
            }
        }
    }
}
