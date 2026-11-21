#include <catch2/catch_test_macros.hpp>

#include "libc/include/featheros/framebuffer.h"
#include "user/gfx_check.h"
#include "user/graphics/display.h"
#include "user/graphics/surface.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Graphics = FeatherOS::Graphics;
using Graphics::Display;
using Graphics::DirtyRegion;
using Graphics::Event;
using Graphics::EventKind;
using Graphics::Status;
using Graphics::Surface;

namespace
{

constexpr uint32_t Red = 0xFF0000;
constexpr uint32_t Green = 0x00FF00;
constexpr uint32_t Blue = 0x0000FF;

size_t Count(const Surface& surface, uint32_t color)
{
    size_t count = 0;
    for (int32_t y = 0; y < surface.Height(); ++y)
    {
        for (int32_t x = 0; x < surface.Width(); ++x)
        {
            count += surface.PixelAt(x, y) == color ? 1 : 0;
        }
    }
    return count;
}

// The rows of the font's '!' glyph; bit x of a row is the pixel x columns from the left.
constexpr uint8_t ExclamationRows[16] = {0x00, 0x00, 0x00, 0x00, 0x08, 0x08, 0x08, 0x08,
                                         0x08, 0x08, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00};

constexpr size_t GlyphPixels = 10;

bool ExclamationPixel(int32_t x, int32_t y)
{
    return (ExclamationRows[y] & (1U << x)) != 0;
}

} // namespace

TEST_CASE("a surface has the size it was asked for or none", "[graphics]")
{
    const Surface surface(5, 3);
    REQUIRE(surface.Valid());
    CHECK(surface.Width() == 5);
    CHECK(surface.Height() == 3);
    CHECK(Count(surface, 0) == 15);
    for (const auto& [width, height] : std::vector<std::pair<int32_t, int32_t>>{
             {0, 4}, {4, 0}, {-1, 4}, {4, -1}, {Graphics::MaximumDimension + 1, 1}})
    {
        const Surface refused(width, height);
        CHECK_FALSE(refused.Valid());
        CHECK(refused.Width() == 0);
        CHECK(refused.PixelAt(0, 0) == 0);
    }
}

TEST_CASE("a surface copies deeply and moves its pixels", "[graphics]")
{
    Surface original(4, 4);
    original.Fill(Red);
    Surface copy(original);
    copy.FillRectangle(0, 0, 2, 2, Blue);
    CHECK(Count(original, Red) == 16);
    CHECK(Count(copy, Blue) == 4);

    Surface assigned(1, 1);
    assigned = original;
    CHECK(assigned.Width() == 4);
    CHECK(Count(assigned, Red) == 16);

    Surface moved(static_cast<Surface&&>(copy));
    CHECK(moved.Valid());
    CHECK_FALSE(copy.Valid());
    CHECK(Count(moved, Blue) == 4);
    Surface other(2, 2);
    other = static_cast<Surface&&>(moved);
    CHECK(other.Width() == 4);
    CHECK_FALSE(moved.Valid());
}

TEST_CASE("fills stay inside the surface", "[graphics]")
{
    Surface surface(10, 8);
    surface.Fill(Red);
    CHECK(Count(surface, Red) == 80);

    SECTION("a rectangle across the top left corner")
    {
        surface.FillRectangle(-3, -2, 5, 4, Green);
        CHECK(Count(surface, Green) == 2 * 2);
        CHECK(surface.PixelAt(1, 1) == Green);
        CHECK(surface.PixelAt(2, 1) == Red);
        CHECK(surface.PixelAt(1, 2) == Red);
    }
    SECTION("a rectangle across the bottom right corner")
    {
        surface.FillRectangle(8, 6, 100, 100, Green);
        CHECK(Count(surface, Green) == 2 * 2);
    }
    SECTION("a rectangle outside, empty or negative draws nothing")
    {
        surface.FillRectangle(10, 0, 4, 4, Green);
        surface.FillRectangle(0, 8, 4, 4, Green);
        surface.FillRectangle(-4, 0, 4, 4, Green);
        surface.FillRectangle(0, -4, 4, 4, Green);
        surface.FillRectangle(2, 2, 0, 3, Green);
        surface.FillRectangle(2, 2, 3, -1, Green);
        CHECK(Count(surface, Green) == 0);
    }
    SECTION("extreme coordinates do not overflow")
    {
        surface.FillRectangle(INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX, Green);
        surface.FillRectangle(INT32_MIN, INT32_MIN, 10, 10, Green);
        CHECK(Count(surface, Green) == 0);
        surface.FillRectangle(INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX, Green);
        CHECK(Count(surface, Green) == 0);
        surface.FillRectangle(-5, -5, INT32_MAX, INT32_MAX, Green);
        CHECK(Count(surface, Green) == 80);
    }
    SECTION("the unused byte of a color is dropped")
    {
        surface.FillRectangle(0, 0, 1, 1, 0xAB123456);
        CHECK(surface.PixelAt(0, 0) == 0x123456);
    }
}

TEST_CASE("rectangle outlines", "[graphics]")
{
    Surface surface(8, 8);

    SECTION("a plain outline")
    {
        surface.DrawRectangle(1, 1, 5, 4, Green);
        CHECK(Count(surface, Green) == 2 * 5 + 2 * 2);
        CHECK(surface.PixelAt(1, 1) == Green);
        CHECK(surface.PixelAt(5, 4) == Green);
        CHECK(surface.PixelAt(3, 2) == 0);
    }
    SECTION("one pixel wide or high is a line")
    {
        surface.DrawRectangle(0, 0, 1, 4, Green);
        CHECK(Count(surface, Green) == 4);
        surface.DrawRectangle(2, 0, 4, 1, Blue);
        CHECK(Count(surface, Blue) == 4);
        surface.DrawRectangle(7, 7, 1, 1, Red);
        CHECK(Count(surface, Red) == 1);
    }
    SECTION("only the visible edges of a clipped outline")
    {
        surface.DrawRectangle(-2, -2, 5, 5, Green);
        CHECK(surface.PixelAt(2, 0) == Green);
        CHECK(surface.PixelAt(2, 2) == Green);
        CHECK(surface.PixelAt(0, 2) == Green);
        CHECK(surface.PixelAt(1, 1) == 0);
        CHECK(Count(surface, Green) == 3 + 2);
    }
    SECTION("an empty outline draws nothing")
    {
        surface.DrawRectangle(1, 1, 0, 3, Green);
        surface.DrawRectangle(1, 1, 3, -2, Green);
        CHECK(Count(surface, Green) == 0);
    }
}

TEST_CASE("blits copy and clip", "[graphics]")
{
    Surface sprite(4, 3);
    for (int32_t y = 0; y < 3; ++y)
    {
        for (int32_t x = 0; x < 4; ++x)
        {
            sprite.FillRectangle(x, y, 1, 1, static_cast<uint32_t>(0x100 + y * 4 + x));
        }
    }
    Surface target(6, 5);
    target.Fill(Red);

    SECTION("inside")
    {
        target.Blit(&sprite, 1, 1);
        CHECK(target.PixelAt(1, 1) == 0x100);
        CHECK(target.PixelAt(4, 3) == 0x100 + 11);
        CHECK(target.PixelAt(0, 1) == Red);
        CHECK(target.PixelAt(5, 1) == Red);
        CHECK(Count(target, Red) == 30 - 12);
    }
    SECTION("past the left and top edges shows the lower right of the sprite")
    {
        target.Blit(&sprite, -2, -1);
        CHECK(target.PixelAt(0, 0) == 0x100 + 4 + 2);
        CHECK(target.PixelAt(1, 1) == 0x100 + 8 + 3);
        CHECK(Count(target, Red) == 30 - 2 * 2);
    }
    SECTION("past the right and bottom edges shows the upper left of the sprite")
    {
        target.Blit(&sprite, 4, 3);
        CHECK(target.PixelAt(4, 3) == 0x100);
        CHECK(target.PixelAt(5, 4) == 0x100 + 4 + 1);
        CHECK(Count(target, Red) == 30 - 2 * 2);
    }
    SECTION("entirely outside, and a missing or empty source")
    {
        const Surface empty(0, 0);
        target.Blit(&sprite, 6, 0);
        target.Blit(&sprite, -4, 0);
        target.Blit(&sprite, 0, 5);
        target.Blit(&sprite, 0, -3);
        target.Blit(&sprite, INT32_MAX, INT32_MIN);
        target.Blit(nullptr, 0, 0);
        target.Blit(&empty, 0, 0);
        target.Blit(&target, 1, 1);
        CHECK(Count(target, Red) == 30);
    }
    SECTION("a transparent color leaves the target showing")
    {
        sprite.FillRectangle(1, 0, 2, 3, Green);
        target.BlitKeyed(&sprite, 1, 1, Green);
        CHECK(target.PixelAt(2, 1) == Red);
        CHECK(target.PixelAt(3, 3) == Red);
        CHECK(target.PixelAt(1, 1) == 0x100);
        CHECK(target.PixelAt(4, 1) == 0x100 + 3);
        CHECK(Count(target, Red) == 30 - 12 + 6);
    }
    SECTION("a transparent color is clipped with the blit and ignores the unused byte")
    {
        sprite.Fill(Green);
        sprite.FillRectangle(0, 0, 1, 1, Blue);
        target.BlitKeyed(&sprite, -1, -1, 0x7F000000 | Green);
        CHECK(Count(target, Blue) == 0);
        CHECK(Count(target, Red) == 30);
        target.BlitKeyed(&sprite, 0, 0, 0x7F000000 | Green);
        CHECK(Count(target, Blue) == 1);
        CHECK(Count(target, Red) == 29);
    }
}

TEST_CASE("text is drawn in the kernel's font", "[graphics]")
{
    Surface surface(40, 40);

    SECTION("a known glyph at scale 1")
    {
        surface.DrawText(3, 5, "!", Green, 1);
        for (int32_t y = 0; y < Graphics::GlyphHeight; ++y)
        {
            for (int32_t x = 0; x < Graphics::GlyphWidth; ++x)
            {
                INFO("pixel " << x << ", " << y);
                CHECK(surface.PixelAt(3 + x, 5 + y) == (ExclamationPixel(x, y) ? Green : 0));
            }
        }
        CHECK(Count(surface, Green) == GlyphPixels);
    }
    SECTION("a known glyph at scale 2")
    {
        surface.DrawText(1, 2, "!", Green, 2);
        for (int32_t y = 0; y < 2 * Graphics::GlyphHeight; ++y)
        {
            for (int32_t x = 0; x < 2 * Graphics::GlyphWidth; ++x)
            {
                INFO("pixel " << x << ", " << y);
                CHECK(surface.PixelAt(1 + x, 2 + y) == (ExclamationPixel(x / 2, y / 2) ? Green : 0));
            }
        }
        CHECK(Count(surface, Green) == GlyphPixels * 4);
    }
    SECTION("glyphs advance by their width, and spaces and other bytes stay blank")
    {
        surface.DrawText(0, 0, "! !\xC3", Green, 1);
        CHECK(Count(surface, Green) == 2 * GlyphPixels);
        CHECK(surface.PixelAt(3, 4) == Green);
        CHECK(surface.PixelAt(16 + 3, 4) == Green);
        CHECK(surface.PixelAt(8 + 3, 4) == 0);
    }
    SECTION("a background fills every cell, spaces too")
    {
        surface.DrawTextOn(0, 0, "! ", Green, Blue, 1);
        CHECK(Count(surface, Blue) == 2 * 8 * 16 - GlyphPixels);
        CHECK(Count(surface, Green) == GlyphPixels);
    }
    SECTION("text is clipped at every edge")
    {
        surface.DrawText(-4, -6, "!", Green, 1);
        CHECK(Count(surface, Green) > 0);
        CHECK(surface.PixelAt(0, 0) == 0);
        surface.DrawText(32, 28, "!!", Blue, 1);
        CHECK(Count(surface, Blue) > 0);
        surface.DrawText(100, 0, "!", Red, 1);
        surface.DrawText(0, 100, "!", Red, 1);
        surface.DrawText(-100, 0, "!", Red, 1);
        surface.DrawText(0, INT32_MIN, "!", Red, 1);
        CHECK(Count(surface, Red) == 0);
    }
    SECTION("a bad text or scale draws nothing")
    {
        surface.DrawText(0, 0, nullptr, Green, 1);
        surface.DrawText(0, 0, "!", Green, 0);
        surface.DrawText(0, 0, "!", Green, -1);
        surface.DrawText(0, 0, "!", Green, Graphics::MaximumScale + 1);
        CHECK(Count(surface, Green) == 0);
    }
    SECTION("the text width")
    {
        CHECK(Surface::TextWidth("abc", 1) == 24);
        CHECK(Surface::TextWidth("abc", 3) == 72);
        CHECK(Surface::TextWidth("", 2) == 0);
        CHECK(Surface::TextWidth("abc", 0) == 0);
        CHECK(Surface::TextWidth(nullptr, 1) == 0);
    }
}

TEST_CASE("the checksum follows the pixels", "[graphics]")
{
    Surface first(3, 2);
    Surface second(3, 2);
    CHECK(first.Checksum() == second.Checksum());
    first.FillRectangle(1, 1, 1, 1, Green);
    CHECK(first.Checksum() != second.Checksum());
    second.FillRectangle(1, 1, 1, 1, Green);
    CHECK(first.Checksum() == second.Checksum());
    Surface empty(0, 0);
    CHECK(empty.Checksum() == 2166136261U);
}

TEST_CASE("the picture gfxcheck draws matches its independent checksum", "[graphics]")
{
    namespace Check = FeatherOS::User::GfxCheck;
    Surface surface(Check::Width, Check::Height);
    surface.Fill(0x101820);
    surface.FillRectangle(4, 4, 30, 20, 0xC03030);
    surface.DrawRectangle(-2, -2, 20, 12, 0xFFFFFF);
    surface.DrawText(6, 6, "Feather", 0xFFFF00, 1);
    surface.DrawText(20, 24, "OS", 0x00FF00, 2);
    Surface sprite(8, 8);
    sprite.Fill(0xFF00FF);
    sprite.FillRectangle(2, 2, 4, 4, 0x00FFFF);
    surface.BlitKeyed(&sprite, 50, 30, 0xFF00FF);
    CHECK(Check::ExpectedChecksum() == 1833110056U);
    CHECK(surface.Checksum() == Check::ExpectedChecksum());
}

namespace
{

struct Rectangles
{
    std::vector<Graphics::Rectangle> list;

    explicit Rectangles(const DirtyRegion& region)
    {
        for (uint32_t index = 0; index < region.Count(); ++index)
        {
            list.push_back(region.At(index));
        }
    }
};

bool Is(const Graphics::Rectangle& rectangle, int32_t x, int32_t y, int32_t width, int32_t height)
{
    return rectangle.x == x && rectangle.y == y && rectangle.width == width &&
           rectangle.height == height;
}

} // namespace

TEST_CASE("dirty rectangles are clipped, merged and capped", "[graphics]")
{
    DirtyRegion region(100, 80);
    CHECK(region.Count() == 0);

    SECTION("a rectangle is clipped to the bounds")
    {
        region.Add(-10, -10, 20, 20);
        REQUIRE(region.Count() == 1);
        CHECK(Is(region.At(0), 0, 0, 10, 10));
    }
    SECTION("nothing outside, empty or negative is kept")
    {
        region.Add(100, 0, 5, 5);
        region.Add(0, 80, 5, 5);
        region.Add(-5, 0, 5, 5);
        region.Add(0, 0, 0, 5);
        region.Add(0, 0, 5, -5);
        region.Add(INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX);
        CHECK(region.Count() == 0);
    }
    SECTION("separate rectangles stay separate")
    {
        region.Add(0, 0, 10, 10);
        region.Add(20, 0, 10, 10);
        const Rectangles all(region);
        REQUIRE(all.list.size() == 2);
        CHECK(Is(all.list[0], 0, 0, 10, 10));
        CHECK(Is(all.list[1], 20, 0, 10, 10));
        CHECK(Is(region.At(2), 0, 0, 0, 0));
    }
    SECTION("overlapping rectangles become their bounding box")
    {
        region.Add(0, 0, 10, 10);
        region.Add(5, 5, 10, 10);
        REQUIRE(region.Count() == 1);
        CHECK(Is(region.At(0), 0, 0, 15, 15));
    }
    SECTION("rectangles that touch are merged")
    {
        region.Add(0, 0, 10, 5);
        region.Add(10, 0, 5, 5);
        REQUIRE(region.Count() == 1);
        CHECK(Is(region.At(0), 0, 0, 15, 5));
    }
    SECTION("a rectangle inside another changes nothing")
    {
        region.Add(0, 0, 30, 10);
        region.Add(5, 2, 3, 3);
        REQUIRE(region.Count() == 1);
        CHECK(Is(region.At(0), 0, 0, 30, 10));
    }
    SECTION("a rectangle bridging two merges both")
    {
        region.Add(0, 0, 5, 5);
        region.Add(30, 0, 5, 5);
        region.Add(5, 0, 25, 5);
        REQUIRE(region.Count() == 1);
        CHECK(Is(region.At(0), 0, 0, 35, 5));
    }
    SECTION("a merge that grows into a neighbor takes it in")
    {
        region.Add(0, 0, 4, 4);
        region.Add(10, 10, 4, 4);
        region.Add(3, 3, 8, 8);
        REQUIRE(region.Count() == 1);
        CHECK(Is(region.At(0), 0, 0, 14, 14));
    }
    SECTION("past the capacity it is the whole of the bounds")
    {
        for (uint32_t index = 0; index < DirtyRegion::Capacity; ++index)
        {
            region.Add(static_cast<int32_t>(index) * 12, 0, 6, 6);
        }
        CHECK(region.Count() == DirtyRegion::Capacity);
        region.Add(0, 40, 2, 2);
        REQUIRE(region.Count() == 1);
        CHECK(Is(region.At(0), 0, 0, 100, 80));
    }
    SECTION("once the rectangles cover half the bounds it is the whole of the bounds")
    {
        region.Add(0, 0, 100, 39);
        CHECK(region.Count() == 1);
        CHECK(Is(region.At(0), 0, 0, 100, 39));
        region.Add(0, 60, 100, 1);
        REQUIRE(region.Count() == 1);
        CHECK(Is(region.At(0), 0, 0, 100, 80));
    }
    SECTION("clearing and adding everything")
    {
        region.Add(0, 0, 3, 3);
        region.Clear();
        CHECK(region.Count() == 0);
        region.Add(0, 0, 3, 3);
        region.AddAll();
        REQUIRE(region.Count() == 1);
        CHECK(Is(region.At(0), 0, 0, 100, 80));
    }
}

TEST_CASE("a region with no bounds stays empty", "[graphics]")
{
    DirtyRegion region(0, 0);
    region.Add(0, 0, 10, 10);
    region.AddAll();
    CHECK(region.Count() == 0);
}

namespace
{

constexpr int ScreenWidth = 64;
constexpr int ScreenHeight = 48;

struct PresentCall
{
    const uint32_t* pixels;
    uint32_t stride;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
};

struct Kernel
{
    bool open_fails = false;
    int open_error = ENOENT;
    bool info_fails = false;
    bool acquire_fails = false;
    int acquire_error = EBUSY;
    bool present_fails = false;
    uint32_t console = 2;
    uint32_t dropped = 0;
    std::vector<uint8_t> queued;
    int read_error = 0;
    long read_length = -2;
    int poll_result = 1;
    int poll_error = 0;
    int polled_timeout = -1;
    int reads = 0;
    int polls = 0;
    int opened = 0;
    int closed = 0;
    int acquired = 0;
    int released = 0;
    std::vector<PresentCall> presents;
};

Kernel Fake;

int FakeOpen(const char* path)
{
    if (std::strcmp(path, "/dev/fb0") != 0)
    {
        errno = ENOENT;
        return -1;
    }
    if (Fake.open_fails)
    {
        errno = Fake.open_error;
        return -1;
    }
    ++Fake.opened;
    return 7;
}

int FakeControl(int descriptor, unsigned long request, void* argument)
{
    REQUIRE(descriptor == 7);
    switch (request)
    {
    case FB_GET_INFO:
    {
        if (Fake.info_fails)
        {
            errno = EIO;
            return -1;
        }
        auto* info = static_cast<fb_info*>(argument);
        *info = {ScreenWidth, ScreenHeight, Fake.console, Fake.dropped};
        return 0;
    }
    case FB_ACQUIRE:
        if (Fake.acquire_fails)
        {
            errno = Fake.acquire_error;
            return -1;
        }
        ++Fake.acquired;
        return 0;
    case FB_PRESENT:
    {
        if (Fake.present_fails)
        {
            errno = EFAULT;
            return -1;
        }
        const auto* request_data = static_cast<const fb_present*>(argument);
        Fake.presents.push_back({request_data->pixels, request_data->stride, request_data->x,
                                 request_data->y, request_data->width, request_data->height});
        REQUIRE(request_data->flags == 0);
        return 0;
    }
    case FB_RELEASE:
        ++Fake.released;
        return 0;
    default:
        errno = ENOTTY;
        return -1;
    }
}

long FakeRead(int descriptor, void* buffer, size_t length)
{
    REQUIRE(descriptor == 7);
    ++Fake.reads;
    if (Fake.read_error != 0)
    {
        errno = Fake.read_error;
        return -1;
    }
    if (Fake.read_length != -2)
    {
        return Fake.read_length;
    }
    if (Fake.queued.empty())
    {
        errno = EAGAIN;
        return -1;
    }
    const size_t taken = std::min(Fake.queued.size(), length / sizeof(fb_event) * sizeof(fb_event));
    std::memcpy(buffer, Fake.queued.data(), taken);
    Fake.queued.erase(Fake.queued.begin(), Fake.queued.begin() + static_cast<long>(taken));
    return static_cast<long>(taken);
}

int FakePoll(int descriptor, int timeout_ms)
{
    REQUIRE(descriptor == 7);
    ++Fake.polls;
    Fake.polled_timeout = timeout_ms;
    if (Fake.poll_error != 0)
    {
        errno = Fake.poll_error;
        return -1;
    }
    return Fake.poll_result;
}

int FakeClose(int descriptor)
{
    REQUIRE(descriptor == 7);
    ++Fake.closed;
    return 0;
}

const Graphics::System FakeSystem = {FakeOpen, FakeControl, FakeRead, FakePoll, FakeClose};

void Queue(uint8_t kind, uint16_t code, uint8_t modifiers, uint32_t time_ms)
{
    const fb_event event = {code, kind, modifiers, time_ms};
    const auto* bytes = reinterpret_cast<const uint8_t*>(&event);
    Fake.queued.insert(Fake.queued.end(), bytes, bytes + sizeof(event));
}

} // namespace

TEST_CASE("a display opens and acquires the framebuffer", "[graphics]")
{
    Fake = Kernel{};
    Display display(&FakeSystem);
    CHECK_FALSE(display.IsOpen());
    CHECK(display.Present() == Status::NotOpen);
    CHECK(display.WaitEvent(0, nullptr) == Status::NotOpen);
    uint32_t dropped = 0;
    CHECK(display.DroppedEvents(&dropped) == Status::NotOpen);

    SECTION("success gives a screen of the reported size")
    {
        REQUIRE(display.Open() == Status::Ok);
        CHECK(display.IsOpen());
        CHECK(display.Width() == ScreenWidth);
        CHECK(display.Height() == ScreenHeight);
        CHECK(display.Console() == 2);
        CHECK(display.Screen()->Width() == ScreenWidth);
        CHECK(Fake.acquired == 1);
        CHECK(display.Open() == Status::Ok);
        CHECK(Fake.opened == 1);
    }
    SECTION("an open that fails")
    {
        Fake.open_fails = true;
        Fake.open_error = EACCES;
        CHECK(display.Open() == Status::OpenFailed);
        CHECK(display.Error() == EACCES);
        CHECK_FALSE(display.IsOpen());
    }
    SECTION("an info request that fails closes the file")
    {
        Fake.info_fails = true;
        CHECK(display.Open() == Status::InfoFailed);
        CHECK(display.Error() == EIO);
        CHECK(Fake.closed == 1);
        CHECK(Fake.released == 0);
    }
    SECTION("an acquire that fails closes the file without a release")
    {
        Fake.acquire_fails = true;
        Fake.acquire_error = ENOTTY;
        CHECK(display.Open() == Status::AcquireFailed);
        CHECK(display.Error() == ENOTTY);
        CHECK(Fake.closed == 1);
        CHECK(Fake.released == 0);
        CHECK_FALSE(display.IsOpen());
    }
}

TEST_CASE("a display releases the screen when it goes away", "[graphics]")
{
    Fake = Kernel{};
    {
        Display display(&FakeSystem);
        REQUIRE(display.Open() == Status::Ok);
    }
    CHECK(Fake.released == 1);
    CHECK(Fake.closed == 1);

    Display display(&FakeSystem);
    REQUIRE(display.Open() == Status::Ok);
    display.Close();
    display.Close();
    CHECK(Fake.released == 2);
    CHECK(Fake.closed == 2);
    CHECK_FALSE(display.IsOpen());
    CHECK(display.Open() == Status::Ok);
    CHECK(Fake.opened == 3);
}

TEST_CASE("a display presents the rectangles it was told about", "[graphics]")
{
    Fake = Kernel{};
    Display display(&FakeSystem);
    REQUIRE(display.Open() == Status::Ok);
    const uint32_t* pixels = display.Screen()->Pixels();

    SECTION("nothing dirty presents nothing")
    {
        CHECK(display.Present() == Status::Ok);
        CHECK(Fake.presents.empty());
    }
    SECTION("a rectangle is presented from its place in the surface")
    {
        display.Invalidate(5, 3, 10, 4);
        CHECK(display.Present() == Status::Ok);
        REQUIRE(Fake.presents.size() == 1);
        const PresentCall& call = Fake.presents[0];
        CHECK(call.pixels == pixels + 3 * ScreenWidth + 5);
        CHECK(call.stride == ScreenWidth);
        CHECK(call.x == 5);
        CHECK(call.y == 3);
        CHECK(call.width == 10);
        CHECK(call.height == 4);
        CHECK(display.Present() == Status::Ok);
        CHECK(Fake.presents.size() == 1);
    }
    SECTION("separate and overlapping rectangles are presented merged")
    {
        display.Invalidate(0, 0, 4, 4);
        display.Invalidate(40, 30, 4, 4);
        display.Invalidate(2, 2, 6, 6);
        CHECK(display.Present() == Status::Ok);
        REQUIRE(Fake.presents.size() == 2);
        CHECK(Fake.presents[0].x == 40);
        CHECK(Fake.presents[1].x == 0);
        CHECK(Fake.presents[1].width == 8);
        CHECK(Fake.presents[1].height == 8);
    }
    SECTION("rectangles past the threshold become the whole screen")
    {
        display.Invalidate(0, 0, ScreenWidth, ScreenHeight / 2);
        REQUIRE(display.Present() == Status::Ok);
        REQUIRE(Fake.presents.size() == 1);
        CHECK(Fake.presents[0].pixels == pixels);
        CHECK(Fake.presents[0].width == ScreenWidth);
        CHECK(Fake.presents[0].height == ScreenHeight);
    }
    SECTION("everything invalid is the whole screen")
    {
        display.InvalidateAll();
        REQUIRE(display.Present() == Status::Ok);
        REQUIRE(Fake.presents.size() == 1);
        CHECK(Fake.presents[0].width == ScreenWidth);
    }
    SECTION("a rectangle off the screen is clipped before it is sent")
    {
        display.Invalidate(-10, ScreenHeight - 2, 20, 20);
        REQUIRE(display.Present() == Status::Ok);
        REQUIRE(Fake.presents.size() == 1);
        CHECK(Fake.presents[0].x == 0);
        CHECK(Fake.presents[0].y == ScreenHeight - 2);
        CHECK(Fake.presents[0].width == 10);
        CHECK(Fake.presents[0].height == 2);
    }
    SECTION("a present that fails keeps the rectangles for the next try")
    {
        display.Invalidate(1, 1, 4, 4);
        Fake.present_fails = true;
        CHECK(display.Present() == Status::PresentFailed);
        CHECK(display.Error() == EFAULT);
        Fake.present_fails = false;
        CHECK(display.Present() == Status::Ok);
        REQUIRE(Fake.presents.size() == 1);
        CHECK(Fake.presents[0].x == 1);
    }
}

TEST_CASE("a display decodes the events it reads", "[graphics]")
{
    Fake = Kernel{};
    Display display(&FakeSystem);
    REQUIRE(display.Open() == Status::Ok);
    Event event = {};

    SECTION("events come out in order, several to a read")
    {
        Queue(FB_EVENT_SHOWN, 0, 0, 100);
        Queue(FB_EVENT_PRESS, KEY_A, FB_MOD_SHIFT | FB_MOD_CAPS_LOCK, 110);
        Queue(FB_EVENT_REPEAT, KEY_A, FB_MOD_SHIFT, 150);
        Queue(FB_EVENT_RELEASE, KEY_A, 0, 190);
        Queue(FB_EVENT_HIDDEN, 0, 0, 200);

        REQUIRE(display.WaitEvent(50, &event) == Status::Ok);
        CHECK(Fake.polled_timeout == 50);
        CHECK(event.kind == EventKind::Shown);
        CHECK(event.time_ms == 100);
        REQUIRE(display.WaitEvent(50, &event) == Status::Ok);
        CHECK(event.kind == EventKind::Press);
        CHECK(event.code == KEY_A);
        CHECK(event.modifiers == (FB_MOD_SHIFT | FB_MOD_CAPS_LOCK));
        CHECK(event.time_ms == 110);
        REQUIRE(display.WaitEvent(50, &event) == Status::Ok);
        CHECK(event.kind == EventKind::Repeat);
        REQUIRE(display.WaitEvent(50, &event) == Status::Ok);
        CHECK(event.kind == EventKind::Release);
        CHECK(event.modifiers == 0);
        REQUIRE(display.WaitEvent(50, &event) == Status::Ok);
        CHECK(event.kind == EventKind::Hidden);
        CHECK(Fake.reads == 1);
        CHECK(Fake.polls == 1);
        CHECK(display.WaitEvent(0, &event) == Status::Timeout);
        CHECK(Fake.polls == 2);
    }
    SECTION("more events than one read holds need a second read")
    {
        for (uint32_t index = 0; index < 20; ++index)
        {
            Queue(FB_EVENT_PRESS, KEY_B, 0, index);
        }
        for (uint32_t index = 0; index < 20; ++index)
        {
            REQUIRE(display.WaitEvent(0, &event) == Status::Ok);
            CHECK(event.time_ms == index);
        }
        CHECK(Fake.reads == 2);
    }
    SECTION("a kind the library does not know")
    {
        Queue(9, 5, 0, 1);
        REQUIRE(display.WaitEvent(0, &event) == Status::Ok);
        CHECK(event.kind == EventKind::Unknown);
        CHECK(event.code == 5);
    }
    SECTION("an event can be skipped without a place for it")
    {
        Queue(FB_EVENT_PRESS, KEY_C, 0, 1);
        CHECK(display.WaitEvent(0, nullptr) == Status::Ok);
        CHECK(display.WaitEvent(0, &event) == Status::Timeout);
    }
    SECTION("a timeout reads nothing")
    {
        Fake.poll_result = 0;
        CHECK(display.WaitEvent(10, &event) == Status::Timeout);
        CHECK(Fake.reads == 0);
    }
    SECTION("a signal in poll or read is reported as such")
    {
        Fake.poll_error = EINTR;
        CHECK(display.WaitEvent(10, &event) == Status::Interrupted);
        Fake.poll_error = 0;
        Fake.read_error = EINTR;
        CHECK(display.WaitEvent(10, &event) == Status::Interrupted);
    }
    SECTION("an empty queue after poll is a timeout")
    {
        CHECK(display.WaitEvent(10, &event) == Status::Timeout);
        CHECK(Fake.reads == 1);
    }
    SECTION("other errors fail with their number")
    {
        Fake.poll_error = EBADF;
        CHECK(display.WaitEvent(10, &event) == Status::ReadFailed);
        CHECK(display.Error() == EBADF);
        Fake.poll_error = 0;
        Fake.read_error = EINVAL;
        CHECK(display.WaitEvent(10, &event) == Status::ReadFailed);
        CHECK(display.Error() == EINVAL);
    }
    SECTION("a read that is not whole events fails")
    {
        Fake.read_length = 5;
        CHECK(display.WaitEvent(10, &event) == Status::ReadFailed);
        Fake.read_length = 0;
        CHECK(display.WaitEvent(10, &event) == Status::ReadFailed);
    }
    SECTION("the dropped count comes from the info request")
    {
        Fake.dropped = 3;
        uint32_t dropped = 0;
        CHECK(display.DroppedEvents(&dropped) == Status::Ok);
        CHECK(dropped == 3);
        Fake.info_fails = true;
        CHECK(display.DroppedEvents(&dropped) == Status::InfoFailed);
    }
}
