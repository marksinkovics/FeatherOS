// SPDX-License-Identifier: MIT OR Apache-2.0
#include "tests/page_cache_support.h"

#include "host/standard_host.h"

#ifdef FEATHERDISK_TEST_PAGE_CACHE
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <memory>
#include <mutex>
#endif

namespace FeatherDisk::Tests
{

std::uint8_t* TestPageSource::AllocatePage()
{
    if (fail)
    {
        return nullptr;
    }
    ++allocations;
    ++outstanding;
    return new std::uint8_t[CachePageSize];
}

void TestPageSource::FreePage(std::uint8_t* page)
{
    --outstanding;
    delete[] page;
}

std::size_t TestPageSource::Headroom() const
{
    const std::size_t used = outstanding;
    return used < budget ? budget - used : 0;
}

RecordingDevice::RecordingDevice(BlockDevice& below) : lower(below) {}

std::uint32_t RecordingDevice::SectorSize() const
{
    return lower.SectorSize();
}

std::uint64_t RecordingDevice::SectorCount() const
{
    return lower.SectorCount();
}

Status RecordingDevice::Read(std::uint64_t first_sector, std::span<std::uint8_t> buffer)
{
    reads.push_back({first_sector, buffer.size() / lower.SectorSize()});
    return lower.Read(first_sector, buffer);
}

Status RecordingDevice::Write(std::uint64_t first_sector, std::span<const std::uint8_t> data)
{
    calls.push_back({false, first_sector, {data.begin(), data.end()}});
    if (fail_writes)
    {
        return std::unexpected(Error::Io);
    }
    return lower.Write(first_sector, data);
}

Status RecordingDevice::Flush()
{
    calls.push_back({true, 0, {}});
    return lower.Flush();
}

bool RecordingDevice::CachesBlocks() const
{
    return lower.CachesBlocks();
}

FrozenClock::FrozenClock()
{
    // 2023-11-14 22:13:20 and 123456789 ns.
    Host::FreezeWallClock(1700000000123456789ULL);
}

FrozenClock::~FrozenClock()
{
    Host::FreezeWallClock(std::nullopt);
}

#ifdef FEATHERDISK_TEST_PAGE_CACHE

namespace
{

constexpr std::size_t SharedLimit = 16;

struct Shared
{
    Shared()
    {
        REQUIRE(cache.Initialize(source, SharedLimit).has_value());
    }

    TestPageSource source;
    PageCache cache;
    std::mutex mutex;
    // A device's wrapper is kept for good and bound afresh on every call, so one left by a
    // device that is gone is never used again.
    std::map<const BlockDevice*, std::unique_ptr<CachedDevice>> wrappers;
};

Shared& SharedCache()
{
    static Shared shared;
    return shared;
}

} // namespace

BlockDevice& Mountable(BlockDevice& device)
{
    Shared& shared = SharedCache();
    const std::scoped_lock guard(shared.mutex);
    std::unique_ptr<CachedDevice>& wrapper = shared.wrappers[&device];
    if (wrapper == nullptr)
    {
        wrapper = std::make_unique<CachedDevice>();
    }
    wrapper->Attach(&shared.cache, device);
    return *wrapper;
}

#else

BlockDevice& Mountable(BlockDevice& device)
{
    return device;
}

#endif

} // namespace FeatherDisk::Tests
