#include <catch2/catch_test_macros.hpp>

#include "fs/page_cache.h"
#include "kernel/memory/page_allocator.h"
#include "kernel/storage/cache_sizing.h"
#include "kernel/storage/page_cache.h"

#include "support/page_cache_host.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace
{

namespace Memory = FeatherOS::Memory;
namespace Storage = FeatherOS::BlockDevice;
using FeatherDisk::CachePageSize;

constexpr std::size_t FrameTotal = HostPageCache::FrameTotal;

std::uint64_t HostOffset()
{
    return HostPageCache::HostOffset();
}

void EnsureAllocator()
{
    REQUIRE(HostPageCache::EnsureAllocator());
}

struct ReclaimerGuard
{
    ~ReclaimerGuard()
    {
        Memory::SetFrameReclaimer(nullptr);
    }
};

std::vector<std::uint64_t> hogged;

void HogAll()
{
    std::uint64_t frame;
    while (Memory::AllocatePageFrame(&frame))
    {
        hogged.push_back(frame);
    }
}

void ReleaseHogged()
{
    for (const std::uint64_t frame : hogged)
    {
        REQUIRE(Memory::FreePageFrame(frame));
    }
    hogged.clear();
}

class RamDevice final : public FeatherDisk::BlockDevice
{
public:
    explicit RamDevice(std::size_t pages) : bytes(pages * CachePageSize)
    {
        for (std::size_t index = 0; index < bytes.size(); ++index)
        {
            bytes[index] = static_cast<std::uint8_t>(index * 7 + index / 4096);
        }
    }

    std::uint32_t SectorSize() const override
    {
        return 512;
    }

    std::uint64_t SectorCount() const override
    {
        return bytes.size() / 512;
    }

    FeatherDisk::Status Read(std::uint64_t first_sector, std::span<std::uint8_t> buffer) override
    {
        std::memcpy(buffer.data(), bytes.data() + first_sector * 512, buffer.size());
        return {};
    }

    FeatherDisk::Status Write(std::uint64_t, std::span<const std::uint8_t>) override
    {
        return {};
    }

    FeatherDisk::Status Flush() override
    {
        return {};
    }

    std::vector<std::uint8_t> bytes;
};

bool ReadPage(FeatherDisk::CachedDevice& device, const RamDevice& ram, std::size_t page)
{
    std::array<std::uint8_t, CachePageSize> buffer{};
    if (!device.Read(page * 8, buffer))
    {
        return false;
    }
    return std::memcmp(buffer.data(), ram.bytes.data() + page * CachePageSize, CachePageSize) ==
           0;
}

class FakeSource final : public FeatherDisk::PageSource
{
public:
    FakeSource(std::size_t free_pages, std::size_t low_watermark)
        : free(free_pages), watermark(low_watermark)
    {
    }

    ~FakeSource()
    {
        for (std::uint8_t* page : live)
        {
            std::free(page);
        }
    }

    std::uint8_t* AllocatePage() override
    {
        if (free == 0)
        {
            return nullptr;
        }
        --free;
        auto* page = static_cast<std::uint8_t*>(std::malloc(CachePageSize));
        live.push_back(page);
        return page;
    }

    void FreePage(std::uint8_t* page) override
    {
        for (std::size_t index = 0; index < live.size(); ++index)
        {
            if (live[index] == page)
            {
                live.erase(live.begin() + static_cast<std::ptrdiff_t>(index));
                std::free(page);
                ++free;
                return;
            }
        }
        FAIL("a page the source did not hand out");
    }

    std::size_t Headroom() const override
    {
        return free > watermark ? free - watermark : 0;
    }

    std::size_t free;
    std::size_t watermark;
    std::vector<std::uint8_t*> live;
};

} // namespace

TEST_CASE("T16 the watermark is a thirty-second of memory but at least 4 MiB",
          "[page-cache][sizing]")
{
    CHECK(Storage::Watermark(16384) == 1024);
    CHECK(Storage::Watermark(32768) == 1024);
    CHECK(Storage::Watermark(65536) == 2048);
    CHECK(Storage::Watermark(524288) == 16384);
    CHECK(Storage::Watermark(0) == 1024);
    CHECK(Storage::TrimMargin(524288) == 8192);
}

TEST_CASE("T16 the limit is a quarter of the free frames, never the part below the watermark",
          "[page-cache][sizing]")
{
    // QEMU's 2 GiB: nearly all of it free at boot.
    CHECK(Storage::CacheLimit(520000, 524288) == 130000);
    // 64 MiB with a third used: the quarter of what is free, not of what exists.
    CHECK(Storage::CacheLimit(10000, 16384) == 2500);
    CHECK(Storage::CacheLimit(10000, 524288) == 0);
    // Close to the watermark only what lies above it can be taken.
    CHECK(Storage::CacheLimit(1100, 16384) == 76);
    CHECK(Storage::CacheLimit(1024, 16384) == 0);
    CHECK(Storage::CacheLimit(500, 16384) == 0);
    CHECK(Storage::CacheLimit(0, 0) == 0);
}

TEST_CASE("T16 Balance frees one page at a time until the source has headroom",
          "[page-cache][sizing]")
{
    FakeSource source(200, 100);
    FeatherDisk::PageCache cache;
    REQUIRE(cache.Initialize(source, 64));
    RamDevice ram(64);
    FeatherDisk::CachedDevice device;
    device.Attach(&cache, ram);
    for (std::size_t page = 0; page < 40; ++page)
    {
        REQUIRE(ReadPage(device, ram, page));
    }
    REQUIRE(cache.Statistics().pages == 40);

    SECTION("with headroom it frees nothing")
    {
        CHECK(cache.Balance() == 0);
        CHECK(cache.Statistics().pages == 40);
    }
    SECTION("below the watermark it stops at the first page of headroom")
    {
        source.free -= 70;
        REQUIRE(source.Headroom() == 0);
        CHECK(cache.Balance() == 11);
        CHECK(source.free == 101);
        CHECK(cache.Statistics().pages == 29);
    }
    SECTION("an empty cache ends it")
    {
        source.free -= 160;
        CHECK(cache.Balance() == 40);
        CHECK(cache.Statistics().pages == 0);
        CHECK(source.Headroom() == 0);
    }
    device.Detach();
}

TEST_CASE("T16 a trim stops exactly at its margin or an empty cache", "[page-cache][sizing]")
{
    FakeSource source(200, 100);
    FeatherDisk::PageCache cache;
    REQUIRE(cache.Initialize(source, 64));
    RamDevice ram(64);
    FeatherDisk::CachedDevice device;
    device.Attach(&cache, ram);
    for (std::size_t page = 0; page < 40; ++page)
    {
        REQUIRE(ReadPage(device, ram, page));
    }

    SECTION("it does not start while the source has headroom")
    {
        source.free -= 30;
        REQUIRE(source.Headroom() == 30);
        CHECK(Storage::TrimToMargin(cache, source, 50) == 0);
        CHECK(cache.Statistics().pages == 40);
    }
    SECTION("it goes on until the margin, not just to the watermark")
    {
        source.free -= 70;
        CHECK(Storage::TrimToMargin(cache, source, 20) == 30);
        CHECK(source.Headroom() == 20);
        CHECK(source.free == 120);
        CHECK(cache.Statistics().pages == 10);
    }
    SECTION("it ends with an empty cache")
    {
        source.free -= 70;
        CHECK(Storage::TrimToMargin(cache, source, 200) == 40);
        CHECK(cache.Statistics().pages == 0);
        CHECK(source.Headroom() == 30);
    }
    device.Detach();
}

// The listener of support/page_cache_host.cpp ran InitializeCache before the first test and
// checked that it registered the reclaimer.
void EnsureCache()
{
    REQUIRE(HostPageCache::EnsureCache());
    Memory::SetFrameReclaimer(Storage::ReclaimFrames);
}

std::uint64_t reclaim_calls;
std::uint64_t reclaim_asked;
bool reclaim_lock_held;
std::uint64_t reclaim_frees;

std::uint64_t CountingReclaimer(std::uint64_t wanted)
{
    ++reclaim_calls;
    reclaim_asked = wanted;
    reclaim_lock_held = Memory::AllocatorLockHeld();
    if (reclaim_lock_held)
    {
        // Freeing a frame would spin on the held lock; fail instead of hanging.
        return 0;
    }
    std::uint64_t freed = 0;
    while (freed < reclaim_frees && !hogged.empty())
    {
        if (!Memory::FreePageFrame(hogged.back()))
        {
            break;
        }
        hogged.pop_back();
        ++freed;
    }
    return freed;
}

void InstallReclaimer(std::uint64_t frees)
{
    reclaim_calls = 0;
    reclaim_asked = 0;
    reclaim_lock_held = true;
    reclaim_frees = frees;
    Memory::SetFrameReclaimer(CountingReclaimer);
}

TEST_CASE("T17 a failed frame allocation calls the reclaimer once, without the lock, and retries",
          "[page-cache][reclaim]")
{
    EnsureAllocator();
    ReclaimerGuard guard;
    const std::uint64_t free_before = Memory::FreeFrameCount();
    std::uint64_t frame = 0;

    SECTION("a frame is freed and the retry takes it")
    {
        HogAll();
        InstallReclaimer(1);
        REQUIRE(Memory::AllocatePageFrame(&frame));
        CHECK(reclaim_calls == 1);
        CHECK(reclaim_asked == 32);
        CHECK_FALSE(reclaim_lock_held);
        hogged.push_back(frame);
    }
    SECTION("a reclaimer that frees nothing fails after one call and one retry")
    {
        HogAll();
        InstallReclaimer(0);
        CHECK_FALSE(Memory::AllocatePageFrame(&frame));
        CHECK(reclaim_calls == 1);
        CHECK_FALSE(reclaim_lock_held);
    }
    SECTION("no reclaim while a frame is free")
    {
        InstallReclaimer(1);
        REQUIRE(Memory::AllocatePageFrame(&frame));
        CHECK(reclaim_calls == 0);
        hogged.push_back(frame);
    }
    SECTION("contiguous allocations do not reclaim")
    {
        HogAll();
        InstallReclaimer(1);
        CHECK_FALSE(Memory::AllocateContiguousPageFrames(1, &frame));
        CHECK(reclaim_calls == 0);
    }
    SECTION("a null reclaimer fails as before")
    {
        HogAll();
        Memory::SetFrameReclaimer(nullptr);
        CHECK_FALSE(Memory::AllocatePageFrame(&frame));
    }
    Memory::SetFrameReclaimer(nullptr);
    ReleaseHogged();
    CHECK(Memory::FreeFrameCount() == free_before);
}

TEST_CASE("T17 the kernel's cache gives its frames back to a failing allocation",
          "[page-cache][reclaim]")
{
    EnsureAllocator();
    ReclaimerGuard guard;
    EnsureCache();
    Memory::SetFrameReclaimer(Storage::ReclaimFrames);
    FeatherDisk::PageCache& cache = Storage::Cache();
    cache.InvalidateAll();
    const std::uint64_t free_before = Memory::FreeFrameCount();
    const FeatherDisk::PageCacheStatistics before = cache.Statistics();
    REQUIRE(before.limit == Storage::CacheLimit(free_before, FrameTotal));

    RamDevice ram(700);
    FeatherDisk::CachedDevice device;
    device.Attach(&cache, ram);
    for (std::size_t page = 0; page < 600; ++page)
    {
        REQUIRE(ReadPage(device, ram, page));
    }
    REQUIRE(cache.Statistics().pages == 600);
    CHECK(Storage::CachedBytes() == 600 * CachePageSize);
    CHECK(Memory::FreeFrameCount() == free_before - 600);

    SECTION("the periodic balance trims to the margin")
    {
        const std::uint64_t watermark = Storage::Watermark(FrameTotal);
        std::uint64_t frame;
        while (Memory::FreeFrameCount() > watermark - 50)
        {
            REQUIRE(Memory::AllocatePageFrame(&frame));
            hogged.push_back(frame);
        }
        Storage::BalanceCache();
        CHECK(Memory::FreeFrameCount() == watermark + Storage::TrimMargin(FrameTotal));
        CHECK(cache.Statistics().pages == 600 - (watermark / 2 + 50));
    }
    SECTION("exhausting the allocator empties the cache and no frame is lost")
    {
        HogAll();
        CHECK(hogged.size() == free_before);
        CHECK(cache.Statistics().pages == 0);
        ReleaseHogged();
        for (std::size_t page = 0; page < 600; ++page)
        {
            REQUIRE(ReadPage(device, ram, page));
        }
    }
    ReleaseHogged();
    device.Detach();
    cache.InvalidateAll();
    CHECK(Memory::FreeFrameCount() == free_before);
}
