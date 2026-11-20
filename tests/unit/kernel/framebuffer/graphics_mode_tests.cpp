#include <catch2/catch_test_macros.hpp>

#include "kernel/framebuffer/graphics_mode.h"

#include <cstdint>

namespace
{

using FeatherOS::Console::Count;
using FeatherOS::GraphicsMode::Clip;
using FeatherOS::GraphicsMode::Clipped;
using FeatherOS::GraphicsMode::Owners;
using FeatherOS::User::FramebufferPresentRequest;
using FeatherOS::User::SyscallErrorBusy;
using FeatherOS::User::SyscallErrorInvalidArgument;

constexpr uint32_t ScreenWidth = 1024;
constexpr uint32_t ScreenHeight = 768;
constexpr int32_t Largest = INT32_MAX;
constexpr int32_t Smallest = INT32_MIN;

FramebufferPresentRequest Request(int32_t x, int32_t y, uint32_t width, uint32_t height,
                                  uint32_t stride, uint32_t flags = 0)
{
    return {0x1000, stride, x, y, width, height, flags};
}

struct Expected
{
    FramebufferPresentRequest request;
    uint64_t result;
    uint32_t screen_x;
    uint32_t screen_y;
    uint32_t width;
    uint32_t height;
    uint64_t source_offset;
};

} // namespace

TEST_CASE("Clip checks the request's sizes, then keeps the visible part and where it starts")
{
    const Expected table[] = {
        {Request(0, 0, 1024, 768, 1024), 0, 0, 0, 1024, 768, 0},
        {Request(0, 0, 0, 10, 0), 0, 0, 0, 0, 0, 0},
        {Request(5, 5, 10, 0, 10), 0, 0, 0, 0, 0, 0},
        {Request(0, 0, 20, 10, 10), SyscallErrorInvalidArgument, 0, 0, 0, 0, 0},
        {Request(0, 0, 10, 10, 16385), SyscallErrorInvalidArgument, 0, 0, 0, 0, 0},
        {Request(0, 0, 20000, 10, 20000), SyscallErrorInvalidArgument, 0, 0, 0, 0, 0},
        {Request(0, 0, 10, 16385, 10), SyscallErrorInvalidArgument, 0, 0, 0, 0, 0},
        {Request(0, 0, 10, 10, 10, 1), SyscallErrorInvalidArgument, 0, 0, 0, 0, 0},
        {Request(0, 0, 0, 0, 0, 1), SyscallErrorInvalidArgument, 0, 0, 0, 0, 0},
        {Request(-10, 0, 20, 4, 20), 0, 0, 0, 10, 4, 10},
        {Request(0, 760, 16, 100, 16), 0, 0, 760, 16, 8, 0},
        {Request(1000, 700, 100, 100, 100), 0, 1000, 700, 24, 68, 0},
        {Request(-5, -10, 20, 30, 30), 0, 0, 0, 15, 20, 305},
        {Request(1023, 767, 50, 50, 64), 0, 1023, 767, 1, 1, 0},
        {Request(1024, 0, 10, 10, 10), 0, 0, 0, 0, 0, 0},
        {Request(0, 768, 10, 10, 10), 0, 0, 0, 0, 0, 0},
        {Request(-20, 0, 20, 10, 20), 0, 0, 0, 0, 0, 0},
        {Request(0, -10, 20, 10, 20), 0, 0, 0, 0, 0, 0},
        {Request(Largest, Largest, 16384, 16384, 16384), 0, 0, 0, 0, 0, 0},
        {Request(Smallest, Smallest, 16384, 16384, 16384), 0, 0, 0, 0, 0, 0},
        {Request(Largest, 0, 16384, 10, 16384), 0, 0, 0, 0, 0, 0},
        {Request(0, Smallest, 10, 16384, 10), 0, 0, 0, 0, 0, 0},
        {Request(-Largest, -Largest, 16384, 16384, 16384), 0, 0, 0, 0, 0, 0},
        {Request(-16383, -16383, 16384, 16384, 16384), 0, 0, 0, 1, 1, 16383ULL * 16384 + 16383},
        {Request(0, -16383, 16384, 16384, 16384), 0, 0, 0, 1024, 1, 16383ULL * 16384},
    };
    for (const Expected& expected : table)
    {
        Clipped clipped = {1, 2, 3, 4, 5};
        INFO("x " << expected.request.x << " y " << expected.request.y << " width "
                  << expected.request.width << " height " << expected.request.height
                  << " stride " << expected.request.stride << " flags " << expected.request.flags);
        REQUIRE(Clip(expected.request, ScreenWidth, ScreenHeight, &clipped) == expected.result);
        REQUIRE(clipped.screen_x == expected.screen_x);
        REQUIRE(clipped.screen_y == expected.screen_y);
        REQUIRE(clipped.width == expected.width);
        REQUIRE(clipped.height == expected.height);
        REQUIRE(clipped.source_offset == expected.source_offset);
    }
}

TEST_CASE("Clip's source offset reaches the last visible pixel inside the request")
{
    for (const int32_t x : {-1000, -5, 0, 500, 1000})
    {
        for (const int32_t y : {-700, -1, 0, 400, 767})
        {
            const FramebufferPresentRequest request = Request(x, y, 300, 200, 350);
            Clipped clipped;
            REQUIRE(Clip(request, ScreenWidth, ScreenHeight, &clipped) == 0);
            if (clipped.width == 0 || clipped.height == 0)
            {
                continue;
            }
            const uint64_t last = clipped.source_offset +
                                  static_cast<uint64_t>(clipped.height - 1) * request.stride +
                                  (clipped.width - 1);
            REQUIRE(last < static_cast<uint64_t>(request.stride) * (request.height - 1) +
                               request.width);
            REQUIRE(clipped.screen_x + clipped.width <= ScreenWidth);
            REQUIRE(clipped.screen_y + clipped.height <= ScreenHeight);
            REQUIRE(static_cast<int64_t>(clipped.screen_x) - x ==
                    static_cast<int64_t>(clipped.source_offset % request.stride));
        }
    }
}

TEST_CASE("an owner takes one console, and a console has one owner")
{
    Owners owners;
    int first;
    int second;
    REQUIRE(owners.Acquire(0, &first) == 0);
    REQUIRE(owners.Acquire(0, &first) == 0);
    REQUIRE(owners.Acquire(0, &second) == SyscallErrorBusy);
    REQUIRE(owners.Acquire(1, &first) == SyscallErrorBusy);
    REQUIRE(owners.Acquire(Count, &second) == SyscallErrorInvalidArgument);
    REQUIRE(owners.Acquire(1, nullptr) == SyscallErrorInvalidArgument);
    REQUIRE(owners.Owns(0, &first));
    REQUIRE_FALSE(owners.Owns(0, &second));
    REQUIRE_FALSE(owners.Owns(1, &first));
    REQUIRE_FALSE(owners.Owns(0, nullptr));
    REQUIRE(owners.Acquired(0));
    REQUIRE_FALSE(owners.Acquired(1));
    REQUIRE(owners.ConsoleOf(&first) == 0);
    REQUIRE(owners.ConsoleOf(&second) == Count);

    REQUIRE(owners.Release(&second) == Count);
    REQUIRE(owners.Acquired(0));
    REQUIRE(owners.Release(&first) == 0);
    REQUIRE(owners.Release(&first) == Count);
    REQUIRE_FALSE(owners.Acquired(0));
    REQUIRE(owners.Acquire(0, &second) == 0);
}

TEST_CASE("the consoles' owners are independent")
{
    Owners owners;
    int tokens[Count];
    for (uint32_t console = 0; console < Count; ++console)
    {
        REQUIRE(owners.Acquire(console, &tokens[console]) == 0);
    }
    for (uint32_t console = 0; console < Count; ++console)
    {
        REQUIRE(owners.Owns(console, &tokens[console]));
        REQUIRE(owners.Owns(console, &tokens[(console + 1) % Count]) == false);
    }
    REQUIRE(owners.Release(&tokens[2]) == 2);
    for (uint32_t console = 0; console < Count; ++console)
    {
        REQUIRE(owners.Acquired(console) == (console != 2));
    }
    REQUIRE(owners.Acquire(2, &tokens[0]) == SyscallErrorBusy);
    REQUIRE(owners.Acquire(2, &tokens[2]) == 0);
}
