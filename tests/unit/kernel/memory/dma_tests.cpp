#include <catch2/catch_test_macros.hpp>

#include "kernel/memory/dma.h"
#include "kernel/memory/page_allocator.h"

#include "support/page_cache_host.h"

#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

namespace
{

namespace Memory = FeatherOS::Memory;
constexpr std::uint64_t PageSize = Memory::PageSize;

// Takes every free frame, so each test builds the free frames it needs by giving some back.
class Hoard
{
public:
    Hoard()
    {
        REQUIRE(HostPageCache::EnsureAllocator());
        Memory::SetFrameReclaimer(nullptr);
        free_before = Memory::FreeFrameCount();
        std::uint64_t frame;
        while (Memory::AllocatePageFrame(&frame))
        {
            held.insert(frame);
        }
    }

    ~Hoard()
    {
        for (const std::uint64_t frame : held)
        {
            REQUIRE(Memory::FreePageFrame(frame));
        }
        CHECK(Memory::FreeFrameCount() == free_before);
    }

    // The lowest frame that starts count consecutive held frames.
    std::uint64_t FindRun(std::uint64_t count) const
    {
        for (const std::uint64_t start : held)
        {
            bool run = true;
            for (std::uint64_t page = 1; page < count && run; ++page)
            {
                run = held.contains(start + page * PageSize);
            }
            if (run)
            {
                return start;
            }
        }
        FAIL("no run of free frames");
        return 0;
    }

    void Release(std::uint64_t first, std::uint64_t count)
    {
        for (std::uint64_t page = 0; page < count; ++page)
        {
            const std::uint64_t frame = first + page * PageSize;
            REQUIRE(held.erase(frame) == 1);
            std::memset(reinterpret_cast<void*>(HostPageCache::HostOffset() + frame), 0xAA,
                        PageSize);
            REQUIRE(Memory::FreePageFrame(frame));
        }
    }

    // Holds the lowest free run again, which must start at first.
    void Take(std::uint64_t first, std::uint64_t count)
    {
        std::uint64_t found;
        REQUIRE(Memory::AllocateContiguousPageFrames(count, &found));
        REQUIRE(found == first);
        for (std::uint64_t page = 0; page < count; ++page)
        {
            held.insert(first + page * PageSize);
        }
    }

    std::set<std::uint64_t> held;
    std::uint64_t free_before = 0;
};

} // namespace

TEST_CASE("T4 contiguous allocation under a limit stays below it and never crosses it",
          "[dma][allocator]")
{
    Hoard hoard;
    const std::uint64_t start = hoard.FindRun(8);
    hoard.Release(start, 8);
    std::uint64_t found = 0;

    SECTION("nothing lies below the first free frame")
    {
        CHECK_FALSE(Memory::AllocateContiguousPageFrames(1, &found, start));
        CHECK_FALSE(Memory::AllocateContiguousPageFrames(1, &found, 0));
        CHECK_FALSE(Memory::AllocateContiguousPageFrames(1, &found, PageSize - 1));
        CHECK(Memory::FreeFrameCount() == 8);
    }
    SECTION("a run ending exactly at the limit fits, one page more does not")
    {
        REQUIRE(Memory::AllocateContiguousPageFrames(1, &found, start + PageSize));
        CHECK(found == start);
        REQUIRE(Memory::FreePageFrame(found));
        CHECK_FALSE(Memory::AllocateContiguousPageFrames(2, &found, start + PageSize));
        REQUIRE(Memory::AllocateContiguousPageFrames(8, &found, start + 8 * PageSize));
        CHECK(found == start);
        CHECK(Memory::FreeFrameCount() == 0);
        for (std::uint64_t page = 0; page < 8; ++page)
        {
            REQUIRE(Memory::FreePageFrame(found + page * PageSize));
        }
        CHECK_FALSE(Memory::AllocateContiguousPageFrames(9, &found, start + 8 * PageSize));
    }
    SECTION("a limit inside a frame excludes that frame")
    {
        const std::uint64_t limit = start + 5 * PageSize + 123;
        CHECK_FALSE(Memory::AllocateContiguousPageFrames(6, &found, limit));
        CHECK(Memory::FreeFrameCount() == 8);
        REQUIRE(Memory::AllocateContiguousPageFrames(5, &found, limit));
        CHECK(found == start);
        CHECK(found + 5 * PageSize <= limit);
        for (std::uint64_t page = 0; page < 5; ++page)
        {
            REQUIRE(Memory::FreePageFrame(found + page * PageSize));
        }
    }
    SECTION("no limit takes the lowest run")
    {
        REQUIRE(Memory::AllocateContiguousPageFrames(3, &found));
        CHECK(found == start);
        for (std::uint64_t page = 0; page < 3; ++page)
        {
            REQUIRE(Memory::FreePageFrame(found + page * PageSize));
        }
    }
    SECTION("a run above held frames is found and checked against the limit")
    {
        hoard.Take(start, 4);
        const std::uint64_t limit = start + 6 * PageSize;
        CHECK_FALSE(Memory::AllocateContiguousPageFrames(3, &found, limit));
        REQUIRE(Memory::AllocateContiguousPageFrames(2, &found, limit));
        CHECK(found == start + 4 * PageSize);
        for (std::uint64_t page = 0; page < 2; ++page)
        {
            REQUIRE(Memory::FreePageFrame(found + page * PageSize));
        }
    }
    SECTION("a count that cannot fit below the limit fails")
    {
        CHECK_FALSE(Memory::AllocateContiguousPageFrames(0, &found, UINT64_MAX));
        CHECK_FALSE(Memory::AllocateContiguousPageFrames(UINT64_MAX / PageSize, &found, UINT64_MAX));
        CHECK_FALSE(Memory::AllocateContiguousPageFrames(2, &found, PageSize));
    }
}

TEST_CASE("T4 DMA buffers are zeroed, contiguous, below the limit and freed whole",
          "[dma][allocator]")
{
    Hoard hoard;
    const std::uint64_t start = hoard.FindRun(8);
    hoard.Release(start, 8);
    const std::uint64_t below = start + 8 * PageSize;
    Memory::Dma::Buffer buffer = {};

    REQUIRE(Memory::Dma::Allocate(PageSize + 1, below, &buffer));
    CHECK(buffer.physical == start);
    CHECK(buffer.physical + 2 * PageSize <= below);
    CHECK(buffer.size == PageSize + 1);
    CHECK(reinterpret_cast<std::uintptr_t>(buffer.address) ==
          HostPageCache::HostOffset() + buffer.physical);
    for (std::uint64_t offset = 0; offset < 2 * PageSize; ++offset)
    {
        REQUIRE(buffer.address[offset] == 0);
    }
    CHECK(Memory::FreeFrameCount() == 6);
    Memory::Dma::Free(buffer);
    CHECK(Memory::FreeFrameCount() == 8);

    SECTION("a limit under the free frames fails and leaves the buffer alone")
    {
        Memory::Dma::Buffer untouched = {0x1234, nullptr, 77};
        CHECK_FALSE(Memory::Dma::Allocate(PageSize, start, &untouched));
        CHECK(untouched.physical == 0x1234);
        CHECK(untouched.size == 77);
        CHECK_FALSE(Memory::Dma::Allocate(2 * PageSize, start + PageSize, &untouched));
        CHECK(Memory::FreeFrameCount() == 8);
    }
    SECTION("empty, null and overflowing requests fail")
    {
        CHECK_FALSE(Memory::Dma::Allocate(0, below, &buffer));
        CHECK_FALSE(Memory::Dma::Allocate(PageSize, below, nullptr));
        CHECK_FALSE(Memory::Dma::Allocate(UINT64_MAX, UINT64_MAX, &buffer));
        CHECK(Memory::FreeFrameCount() == 8);
    }
    SECTION("the last bytes before the limit are usable")
    {
        REQUIRE(Memory::Dma::Allocate(8 * PageSize, below, &buffer));
        CHECK(buffer.physical == start);
        CHECK(Memory::FreeFrameCount() == 0);
        Memory::Dma::Free(buffer);
    }
}
