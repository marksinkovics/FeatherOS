#include <catch2/catch_test_macros.hpp>

#include "kernel/thread_stacks.h"

namespace
{

constexpr uint64_t StackTop = 0x700000100000;
constexpr uint64_t RegionPages = 64;
constexpr uint64_t Region = RegionPages * FeatherOS::ThreadStacks::PageSize;
constexpr uint64_t Page = FeatherOS::ThreadStacks::PageSize;

} // namespace

TEST_CASE("thread stack slots stack downward from the main stack")
{
    using FeatherOS::ThreadStacks::Bottom;
    using FeatherOS::ThreadStacks::Top;
    REQUIRE(Top(StackTop, RegionPages, 0) == StackTop);
    REQUIRE(Top(StackTop, RegionPages, 1) == StackTop - Region);
    REQUIRE(Top(StackTop, RegionPages, 15) == StackTop - 15 * Region);
    REQUIRE(Bottom(StackTop, RegionPages, 16) == StackTop - 16 * Region);
}

TEST_CASE("an address maps to its slot unless it is a guard page or outside every slot")
{
    using FeatherOS::ThreadStacks::Find;
    uint32_t slot = 99;
    REQUIRE(Find(StackTop, RegionPages, 16, StackTop - 1, &slot));
    REQUIRE(slot == 0);
    REQUIRE(Find(StackTop, RegionPages, 16, StackTop - Region + Page, &slot));
    REQUIRE(slot == 0);
    // Each region's lowest page is its guard, between it and the region below.
    REQUIRE_FALSE(Find(StackTop, RegionPages, 16, StackTop - Region + Page - 1, &slot));
    REQUIRE_FALSE(Find(StackTop, RegionPages, 16, StackTop - Region, &slot));
    REQUIRE(Find(StackTop, RegionPages, 16, StackTop - Region - 1, &slot));
    REQUIRE(slot == 1);
    REQUIRE(Find(StackTop, RegionPages, 16, StackTop - 15 * Region - 1, &slot));
    REQUIRE(slot == 15);
    REQUIRE_FALSE(Find(StackTop, RegionPages, 16, StackTop - 16 * Region + Page - 1, &slot));
    REQUIRE_FALSE(Find(StackTop, RegionPages, 16, StackTop - 16 * Region - 1, &slot));
    REQUIRE_FALSE(Find(StackTop, RegionPages, 16, StackTop, &slot));
    // With fewer slots in use, the regions below them are not stack.
    REQUIRE_FALSE(Find(StackTop, RegionPages, 1, StackTop - Region - 1, &slot));
}
