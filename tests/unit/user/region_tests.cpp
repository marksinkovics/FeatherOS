#include <catch2/catch_test_macros.hpp>

#include "user/graphics/region.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

using FeatherOS::Graphics::Rectangle;
using FeatherOS::Graphics::Region;

namespace
{

constexpr int32_t Low = -16;
constexpr int32_t Extent = 112;

using Bitmap = std::array<bool, static_cast<size_t>(Extent) * Extent>;

size_t Slot(int32_t x, int32_t y)
{
    return static_cast<size_t>(y - Low) * Extent + static_cast<size_t>(x - Low);
}

void Paint(Bitmap& bitmap, const Rectangle& rectangle, bool value)
{
    for (int64_t y = rectangle.y; y < static_cast<int64_t>(rectangle.y) + rectangle.height; ++y)
    {
        for (int64_t x = rectangle.x; x < static_cast<int64_t>(rectangle.x) + rectangle.width; ++x)
        {
            if (x >= Low && x < Low + Extent && y >= Low && y < Low + Extent)
            {
                bitmap[Slot(static_cast<int32_t>(x), static_cast<int32_t>(y))] = value;
            }
        }
    }
}

Bitmap PixelsOf(const Region& region)
{
    Bitmap bitmap{};
    for (size_t index = 0; index < region.RectangleCount(); ++index)
    {
        Paint(bitmap, region.RectangleAt(index), true);
    }
    return bitmap;
}

// The flat list is canonical when it reads as sorted, separated bands, no two touching bands
// share their spans, and nothing is empty.
bool IsCanonical(const Region& region)
{
    for (size_t index = 0; index < region.RectangleCount(); ++index)
    {
        const Rectangle rectangle = region.RectangleAt(index);
        if (rectangle.width <= 0 || rectangle.height <= 0)
        {
            return false;
        }
        if (index == 0)
        {
            continue;
        }
        const Rectangle previous = region.RectangleAt(index - 1);
        if (rectangle.y == previous.y)
        {
            if (rectangle.height != previous.height || rectangle.x <= previous.x + previous.width)
            {
                return false;
            }
            continue;
        }
        if (rectangle.y < previous.y + previous.height)
        {
            return false;
        }
    }

    for (size_t first = 0; first < region.RectangleCount();)
    {
        const Rectangle head = region.RectangleAt(first);
        size_t next = first;
        while (next < region.RectangleCount() && region.RectangleAt(next).y == head.y)
        {
            ++next;
        }
        if (next < region.RectangleCount())
        {
            const Rectangle following = region.RectangleAt(next);
            if (following.y == head.y + head.height)
            {
                size_t end = next;
                while (end < region.RectangleCount() && region.RectangleAt(end).y == following.y)
                {
                    ++end;
                }
                bool same = end - next == next - first;
                for (size_t offset = 0; same && offset < next - first; ++offset)
                {
                    const Rectangle left = region.RectangleAt(first + offset);
                    const Rectangle right = region.RectangleAt(next + offset);
                    same = left.x == right.x && left.width == right.width;
                }
                if (same)
                {
                    return false;
                }
            }
        }
        first = next;
    }
    return true;
}

class Random
{
public:
    explicit Random(uint64_t seed) : m_state(seed) {}

    int32_t Between(int32_t low, int32_t high)
    {
        m_state ^= m_state << 13;
        m_state ^= m_state >> 7;
        m_state ^= m_state << 17;
        return low + static_cast<int32_t>(m_state % static_cast<uint64_t>(high - low + 1));
    }

private:
    uint64_t m_state;
};

Rectangle RandomRectangle(Random& random)
{
    return {random.Between(-8, 72), random.Between(-8, 72), random.Between(-2, 40),
            random.Between(-2, 40)};
}

Region RandomRegion(Random& random, Bitmap& model)
{
    Region region;
    const int32_t count = random.Between(0, 3);
    for (int32_t index = 0; index < count; ++index)
    {
        const Rectangle rectangle = RandomRectangle(random);
        region.Add(rectangle);
        Paint(model, rectangle, true);
    }
    return region;
}

} // namespace

TEST_CASE("region random operations match a bitmap model", "[region]")
{
    Random random(0x5EED0001);
    Region region;
    Bitmap model{};

    for (int step = 0; step < 20000; ++step)
    {
        const int32_t kind = random.Between(0, 9);
        if (kind <= 2)
        {
            const Rectangle rectangle = RandomRectangle(random);
            region.Add(rectangle);
            Paint(model, rectangle, true);
        }
        else if (kind == 3)
        {
            Bitmap other_model{};
            const Region other = RandomRegion(random, other_model);
            region.Add(other);
            for (size_t slot = 0; slot < model.size(); ++slot)
            {
                model[slot] = model[slot] || other_model[slot];
            }
        }
        else if (kind <= 5)
        {
            const Rectangle rectangle = RandomRectangle(random);
            region.Subtract(rectangle);
            Paint(model, rectangle, false);
        }
        else if (kind == 6)
        {
            Bitmap other_model{};
            const Region other = RandomRegion(random, other_model);
            region.Subtract(other);
            for (size_t slot = 0; slot < model.size(); ++slot)
            {
                model[slot] = model[slot] && !other_model[slot];
            }
        }
        else if (kind == 7)
        {
            const Rectangle rectangle = RandomRectangle(random);
            Bitmap keep{};
            Paint(keep, rectangle, true);
            region.Intersect(rectangle);
            for (size_t slot = 0; slot < model.size(); ++slot)
            {
                model[slot] = model[slot] && keep[slot];
            }
        }
        else if (kind == 8)
        {
            Bitmap other_model{};
            const Region other = RandomRegion(random, other_model);
            region.Intersect(other);
            for (size_t slot = 0; slot < model.size(); ++slot)
            {
                model[slot] = model[slot] && other_model[slot];
            }
        }
        else
        {
            const int32_t dx = random.Between(-4, 4);
            const int32_t dy = random.Between(-4, 4);
            region.Translate(dx, dy);
            REQUIRE(IsCanonical(region));
            region.Translate(-dx, -dy);
        }

        REQUIRE(IsCanonical(region));
        REQUIRE(PixelsOf(region) == model);
        const int32_t x = random.Between(-12, 90);
        const int32_t y = random.Between(-12, 90);
        const bool inside = x >= Low && x < Low + Extent && y >= Low && y < Low + Extent;
        REQUIRE(region.Contains(x, y) == (inside && model[Slot(x, y)]));
    }
}

TEST_CASE("region equal pixel sets are equal member by member", "[region]")
{
    Region built_by_rows;
    built_by_rows.Add({0, 0, 10, 2});
    built_by_rows.Add({0, 2, 10, 3});

    Region built_by_columns;
    built_by_columns.Add({5, 0, 5, 5});
    built_by_columns.Add({0, 0, 5, 5});

    REQUIRE(built_by_rows == built_by_columns);
    REQUIRE(built_by_rows.RectangleCount() == 1);
    REQUIRE(built_by_rows.Bounds().width == 10);
    REQUIRE(built_by_rows.Bounds().height == 5);
}

TEST_CASE("region keeps bands with different spans apart", "[region]")
{
    Region region;
    region.Add({0, 0, 4, 2});
    region.Add({0, 2, 6, 2});
    REQUIRE(region.RectangleCount() == 2);

    Region gap;
    gap.Add({0, 0, 4, 2});
    gap.Add({0, 3, 4, 2});
    REQUIRE(gap.RectangleCount() == 2);
}

TEST_CASE("region subtract making a hole gives four rectangles", "[region]")
{
    Region region({0, 0, 10, 10});
    region.Subtract(Rectangle{3, 3, 4, 4});

    REQUIRE(region.RectangleCount() == 4);
    const std::array<Rectangle, 4> expected = {{{0, 0, 10, 3}, {0, 3, 3, 4}, {7, 3, 3, 4}, {0, 7, 10, 3}}};
    for (size_t index = 0; index < expected.size(); ++index)
    {
        const Rectangle actual = region.RectangleAt(index);
        REQUIRE(actual.x == expected[index].x);
        REQUIRE(actual.y == expected[index].y);
        REQUIRE(actual.width == expected[index].width);
        REQUIRE(actual.height == expected[index].height);
    }
    REQUIRE_FALSE(region.Contains(5, 5));
    REQUIRE(region.Contains(2, 5));
    REQUIRE(region.Contains(7, 5));

    region.Add({3, 3, 4, 4});
    REQUIRE(region.RectangleCount() == 1);
}

TEST_CASE("region translate and bounds", "[region]")
{
    Region region;
    REQUIRE(region.Empty());
    REQUIRE(region.Bounds().width == 0);
    REQUIRE(region.Bounds().height == 0);

    region.Add({2, 3, 4, 5});
    region.Add({10, 1, 2, 2});
    Rectangle bounds = region.Bounds();
    REQUIRE(bounds.x == 2);
    REQUIRE(bounds.y == 1);
    REQUIRE(bounds.width == 10);
    REQUIRE(bounds.height == 7);

    region.Translate(-5, 7);
    bounds = region.Bounds();
    REQUIRE(bounds.x == -3);
    REQUIRE(bounds.y == 8);
    REQUIRE(bounds.width == 10);
    REQUIRE(bounds.height == 7);
    REQUIRE(region.Contains(-3, 10));
    REQUIRE_FALSE(region.Contains(2, 10));

    region.Clear();
    REQUIRE(region.Empty());
    REQUIRE(region.RectangleCount() == 0);
    REQUIRE(region.RectangleAt(0).width == 0);
}

TEST_CASE("region combines a region with itself", "[region]")
{
    Region region;
    region.Add({0, 0, 5, 5});
    region.Add({8, 2, 3, 6});
    const Region copy = region;

    region.Add(region);
    REQUIRE(region == copy);
    region.Intersect(region);
    REQUIRE(region == copy);
    region.Subtract(region);
    REQUIRE(region.Empty());
}

TEST_CASE("region clamps extreme coordinates to the surface limit", "[region]")
{
    constexpr int32_t Minimum = std::numeric_limits<int32_t>::min();
    constexpr int32_t Maximum = std::numeric_limits<int32_t>::max();

    Region everything({-5, -5, Maximum, Maximum});
    Rectangle bounds = everything.Bounds();
    REQUIRE(bounds.x == -5);
    REQUIRE(bounds.y == -5);
    REQUIRE(bounds.x + bounds.width == Region::Limit);
    REQUIRE(bounds.y + bounds.height == Region::Limit);

    Region below({Minimum, Minimum, Maximum, Maximum});
    REQUIRE(below.RectangleCount() == 1);
    REQUIRE(below.Bounds().x == -Region::Limit);
    REQUIRE(below.Bounds().x + below.Bounds().width == -1);

    Region beyond({Maximum, Maximum, 10, 10});
    REQUIRE(beyond.Empty());
    Region negative({0, 0, Minimum, 5});
    REQUIRE(negative.Empty());
    Region wide({Minimum, 0, 10, 10});
    REQUIRE(wide.Empty());

    Region moved({0, 0, 10, 10});
    moved.Translate(Maximum, Maximum);
    REQUIRE(moved.Empty());
    moved.Add({0, 0, 10, 10});
    moved.Translate(Minimum, Minimum);
    REQUIRE(moved.Empty());

    Region edge({0, 0, 10, 10});
    edge.Translate(Region::Limit - 4, 0);
    REQUIRE(edge.RectangleCount() == 1);
    REQUIRE(edge.Bounds().width == 4);
    REQUIRE(edge.Contains(Region::Limit - 1, 0));
    REQUIRE_FALSE(edge.Contains(Maximum, 0));
    REQUIRE_FALSE(edge.Contains(Minimum, Minimum));

    everything.Subtract(Rectangle{Minimum, Minimum, Maximum, Maximum});
    REQUIRE(everything.RectangleCount() == 2);
    everything.Subtract(Rectangle{Minimum, Minimum, Maximum, Maximum});
    REQUIRE(IsCanonical(everything));
}
