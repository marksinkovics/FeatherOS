// SPDX-License-Identifier: MIT OR Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "fs/page_cache.h"
#include "fs/partition.h"
#include "tests/memory_device.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <span>
#include <thread>
#include <vector>

namespace
{

using FeatherDisk::BlockDevice;
using FeatherDisk::CachedDevice;
using FeatherDisk::CachePageSize;
using FeatherDisk::Error;
using FeatherDisk::PageCache;
using FeatherDisk::PageSource;
using FeatherDisk::Status;
using FeatherDisk::Partition::Range;
using FeatherDisk::Partition::Volume;
using FeatherDisk::Tests::MemoryDevice;

constexpr std::uint32_t SectorBytes = 512;
constexpr std::uint32_t SectorsPerPage = CachePageSize / SectorBytes;

class TestPageSource final : public PageSource
{
public:
    std::uint8_t* AllocatePage() override
    {
        if (fail)
        {
            return nullptr;
        }
        ++allocations;
        ++outstanding;
        return new std::uint8_t[CachePageSize];
    }

    void FreePage(std::uint8_t* page) override
    {
        --outstanding;
        delete[] page;
    }

    std::size_t Headroom() const override
    {
        const std::size_t used = outstanding;
        return used < budget ? budget - used : 0;
    }

    std::atomic<std::size_t> outstanding{0};
    std::atomic<std::size_t> allocations{0};
    std::atomic<std::size_t> budget{SIZE_MAX};
    std::atomic<bool> fail{false};
};

class RecordingDevice final : public BlockDevice
{
public:
    struct Call
    {
        bool is_flush;
        std::uint64_t first_sector;
        std::vector<std::uint8_t> bytes;
        bool operator==(const Call&) const = default;
    };

    explicit RecordingDevice(BlockDevice& below) : lower(below) {}

    std::uint32_t SectorSize() const override
    {
        return lower.SectorSize();
    }

    std::uint64_t SectorCount() const override
    {
        return lower.SectorCount();
    }

    Status Read(std::uint64_t first_sector, std::span<std::uint8_t> buffer) override
    {
        return lower.Read(first_sector, buffer);
    }

    Status Write(std::uint64_t first_sector, std::span<const std::uint8_t> data) override
    {
        calls.push_back({false, first_sector, {data.begin(), data.end()}});
        return lower.Write(first_sector, data);
    }

    Status Flush() override
    {
        calls.push_back({true, 0, {}});
        return lower.Flush();
    }

    std::vector<Call> calls;

private:
    BlockDevice& lower;
};

// One lock around a device that is not safe to share between threads.
class LockedDevice final : public BlockDevice
{
public:
    explicit LockedDevice(BlockDevice& below) : lower(below) {}

    std::uint32_t SectorSize() const override
    {
        return lower.SectorSize();
    }

    std::uint64_t SectorCount() const override
    {
        return lower.SectorCount();
    }

    Status Read(std::uint64_t first_sector, std::span<std::uint8_t> buffer) override
    {
        const std::scoped_lock guard(mutex);
        return lower.Read(first_sector, buffer);
    }

    Status Write(std::uint64_t first_sector, std::span<const std::uint8_t> data) override
    {
        const std::scoped_lock guard(mutex);
        return lower.Write(first_sector, data);
    }

    Status Flush() override
    {
        const std::scoped_lock guard(mutex);
        return lower.Flush();
    }

private:
    BlockDevice& lower;
    std::mutex mutex;
};

void Fill(MemoryDevice& disk, unsigned seed)
{
    std::span<std::uint8_t> bytes = disk.Bytes();
    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
        bytes[index] = static_cast<std::uint8_t>(index * 131 + (index >> 7) * 17 + seed * 29 + 1);
    }
}

std::vector<std::uint8_t> Slice(MemoryDevice& disk, std::uint64_t first, std::uint64_t count)
{
    const std::uint32_t size = disk.SectorSize();
    const std::span<std::uint8_t> bytes = disk.Bytes().subspan(first * size, count * size);
    return {bytes.begin(), bytes.end()};
}

Status ReadSectors(BlockDevice& device, std::uint64_t first, std::uint64_t count,
                   std::vector<std::uint8_t>& out)
{
    out.assign(count * device.SectorSize(), 0xEE);
    return device.Read(first, out);
}

struct Rig
{
    explicit Rig(std::size_t limit = 8, std::uint64_t sectors = 64,
                 std::uint32_t sector_size = SectorBytes)
        : disk(sector_size, sectors), volume(disk, Range{0, sectors})
    {
        Fill(disk, 1);
        REQUIRE(cache.Initialize(source, limit).has_value());
        cached.Attach(&cache, volume);
    }

    MemoryDevice disk;
    TestPageSource source;
    PageCache cache;
    Volume volume;
    CachedDevice cached;
};

} // namespace

TEST_CASE("page cache: a second read of a range makes no device read", "[page_cache]")
{
    Rig rig;
    std::vector<std::uint8_t> first;
    std::vector<std::uint8_t> second;
    REQUIRE(ReadSectors(rig.cached, 3, 8, first).has_value());
    REQUIRE(first == Slice(rig.disk, 3, 8));
    const std::uint64_t reads = rig.disk.read_count;
    REQUIRE(reads > 0);

    REQUIRE(ReadSectors(rig.cached, 3, 8, second).has_value());
    REQUIRE(second == first);
    REQUIRE(rig.disk.read_count == reads);
    REQUIRE(ReadSectors(rig.cached, 5, 2, second).has_value());
    REQUIRE(second == Slice(rig.disk, 5, 2));
    REQUIRE(rig.disk.read_count == reads);
    REQUIRE(rig.cache.Statistics().hits >= 3);
}

TEST_CASE("page cache: the last page is short and nothing outside the volume is read",
          "[page_cache]")
{
    constexpr std::uint64_t Base = 8;
    constexpr std::uint64_t VolumeSectors = 8 * 3 + 3;
    MemoryDevice disk(SectorBytes, 64);
    Fill(disk, 2);
    Volume volume(disk, Range{Base, VolumeSectors});
    TestPageSource source;
    PageCache cache;
    REQUIRE(cache.Initialize(source, 8).has_value());
    CachedDevice cached;
    cached.Attach(&cache, volume);

    bool outside = false;
    disk.on_read = [&](std::uint64_t first, std::uint64_t count)
    { outside = outside || first < Base || first + count > Base + VolumeSectors; };

    std::vector<std::uint8_t> got;
    REQUIRE(ReadSectors(cached, VolumeSectors - 3, 3, got).has_value());
    REQUIRE(got == Slice(disk, Base + VolumeSectors - 3, 3));
    REQUIRE(ReadSectors(cached, 0, VolumeSectors, got).has_value());
    REQUIRE(got == Slice(disk, Base, VolumeSectors));
    REQUIRE(ReadSectors(cached, 20, 7, got).has_value());
    REQUIRE(got == Slice(disk, Base + 20, 7));
    REQUIRE_FALSE(outside);

    const std::uint64_t reads = disk.read_count;
    REQUIRE(ReadSectors(cached, VolumeSectors - 1, 1, got).has_value());
    REQUIRE(got == Slice(disk, Base + VolumeSectors - 1, 1));
    REQUIRE(disk.read_count == reads);

    REQUIRE(ReadSectors(cached, VolumeSectors - 1, 2, got).error() == Error::InvalidArgument);
    REQUIRE(ReadSectors(cached, VolumeSectors + 1, 0, got).error() == Error::InvalidArgument);
    REQUIRE_FALSE(outside);
}

TEST_CASE("page cache: pages leave in least recently used order", "[page_cache]")
{
    constexpr std::size_t Limit = 3;
    Rig rig(Limit);
    std::vector<std::uint64_t> lru;
    std::mt19937 random(7);
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;
    std::vector<std::uint8_t> got;
    for (unsigned access = 0; access < 300; ++access)
    {
        const std::uint64_t page = random() % 6;
        const std::uint64_t sector = page * SectorsPerPage + access % SectorsPerPage;
        const std::uint64_t before = rig.disk.read_count;
        REQUIRE(ReadSectors(rig.cached, sector, 1, got).has_value());
        REQUIRE(got == Slice(rig.disk, sector, 1));

        const auto found = std::ranges::find(lru, page);
        const bool miss = found == lru.end();
        REQUIRE(rig.disk.read_count - before == (miss ? 1 : 0));
        if (miss)
        {
            ++misses;
            if (lru.size() == Limit)
            {
                lru.pop_back();
                ++evictions;
            }
        }
        else
        {
            ++hits;
            lru.erase(found);
        }
        lru.insert(lru.begin(), page);
        REQUIRE(rig.cache.Statistics().pages == lru.size());
    }
    const auto statistics = rig.cache.Statistics();
    REQUIRE(statistics.hits == hits);
    REQUIRE(statistics.misses == misses);
    REQUIRE(statistics.evictions == evictions);
    REQUIRE(rig.source.outstanding == Limit);
}

TEST_CASE("page cache: random reads, writes and drops match a byte model", "[page_cache]")
{
    for (const std::uint32_t sector_size : {512U, 2048U})
    {
        const std::uint32_t per_page = CachePageSize / sector_size;
        const std::uint64_t sectors = sector_size == 512 ? 100 : 25;
        MemoryDevice disk(sector_size, sectors);
        Fill(disk, 3);
        std::vector<std::uint8_t> model(disk.Bytes().begin(), disk.Bytes().end());
        TestPageSource source;
        PageCache cache;
        REQUIRE(cache.Initialize(source, 4).has_value());

        const std::array<Range, 3> ranges =
            sector_size == 512 ? std::array{Range{2, 29}, Range{35, 29}, Range{66, 31}}
                               : std::array{Range{0, 7}, Range{8, 7}, Range{16, 8}};
        std::array<Volume, 3> volumes;
        std::array<CachedDevice, 3> devices;
        for (std::size_t index = 0; index < 3; ++index)
        {
            volumes[index].Assign(disk, ranges[index]);
            devices[index].Attach(&cache, volumes[index]);
        }

        std::mt19937 random(11 + sector_size);
        std::vector<std::uint8_t> got;
        for (unsigned step = 0; step < 4000; ++step)
        {
            const std::size_t which = random() % 3;
            const Range range = ranges[which];
            const unsigned action = random() % 100;
            const std::uint64_t first = random() % range.sector_count;
            const std::uint64_t count =
                1 + random() % std::min<std::uint64_t>(3 * per_page, range.sector_count - first);
            if (action < 55)
            {
                REQUIRE(ReadSectors(devices[which], first, count, got).has_value());
                const auto from = model.begin() + (range.first_sector + first) * sector_size;
                REQUIRE(std::equal(got.begin(), got.end(), from));
            }
            else if (action < 85)
            {
                std::vector<std::uint8_t> bytes(count * sector_size);
                for (std::uint8_t& byte : bytes)
                {
                    byte = static_cast<std::uint8_t>(random());
                }
                REQUIRE(devices[which].Write(first, bytes).has_value());
                std::ranges::copy(bytes,
                                  model.begin() + (range.first_sector + first) * sector_size);
                REQUIRE(std::ranges::equal(disk.Bytes(), model));
            }
            else if (action < 90)
            {
                devices[which].Invalidate();
            }
            else if (action < 95)
            {
                cache.Shrink(random() % 4);
            }
            else if (action < 98)
            {
                devices[which].Detach();
                devices[which].Attach(&cache, volumes[which]);
            }
            else
            {
                cache.InvalidateAll();
            }
            REQUIRE(cache.Statistics().pages <= 4);
            REQUIRE(cache.Statistics().pages == source.outstanding);
        }
    }
}

TEST_CASE("page cache: writes and flushes reach the device as called", "[page_cache]")
{
    MemoryDevice disk(SectorBytes, 64);
    Fill(disk, 4);
    RecordingDevice recording(disk);
    TestPageSource source;
    PageCache cache;
    REQUIRE(cache.Initialize(source, 4).has_value());
    CachedDevice cached;
    cached.Attach(&cache, recording);

    std::vector<RecordingDevice::Call> expected;
    std::vector<std::uint8_t> got;
    const auto write = [&](std::uint64_t first, std::size_t sectors, std::uint8_t value)
    {
        const std::vector<std::uint8_t> bytes(sectors * SectorBytes, value);
        REQUIRE(cached.Write(first, bytes).has_value());
        expected.push_back({false, first, bytes});
        REQUIRE(Slice(disk, first, sectors) == bytes);
    };
    const auto flush = [&]
    {
        REQUIRE(cached.Flush().has_value());
        expected.push_back({true, 0, {}});
    };

    write(0, 1, 0x11);
    REQUIRE(ReadSectors(cached, 0, 16, got).has_value());
    write(2, 3, 0x22);
    flush();
    write(7, 4, 0x33);
    write(40, 2, 0x44);
    flush();
    flush();
    REQUIRE(ReadSectors(cached, 0, 16, got).has_value());
    write(5, 1, 0x55);
    REQUIRE(ReadSectors(cached, 0, 16, got).has_value());
    REQUIRE(got == Slice(disk, 0, 16));
    flush();
    write(60, 4, 0x66);

    REQUIRE(recording.calls == expected);
}

TEST_CASE("page cache: a failed write drops the pages it touched", "[page_cache]")
{
    Rig rig;
    std::vector<std::uint8_t> got;
    REQUIRE(ReadSectors(rig.cached, 0, 16, got).has_value());
    REQUIRE(rig.cache.Statistics().pages == 2);

    rig.disk.FailWritesAfter(rig.disk.write_count);
    const std::vector<std::uint8_t> bytes(4 * SectorBytes, 0x77);
    REQUIRE(rig.cached.Write(6, bytes).error() == Error::Io);
    REQUIRE(rig.cache.Statistics().pages == 0);
    REQUIRE(rig.cache.Statistics().invalidated == 2);
    // A partly done write leaves the device in an unknown state; it is the device that is read.
    rig.disk.Sector(7)[0] ^= 0xFF;
    const std::uint64_t reads = rig.disk.read_count;
    REQUIRE(ReadSectors(rig.cached, 6, 2, got).has_value());
    REQUIRE(rig.disk.read_count > reads);
    REQUIRE(got == Slice(rig.disk, 6, 2));
}

TEST_CASE("page cache: read errors are the device's, and nothing failed is cached", "[page_cache]")
{
    Rig rig;
    std::vector<std::uint8_t> got;

    rig.disk.FailReadsAt(5);
    REQUIRE(ReadSectors(rig.cached, 2, 1, got).has_value());
    REQUIRE(got == Slice(rig.disk, 2, 1));
    REQUIRE(rig.cache.Statistics().pages == 0);
    REQUIRE(rig.source.outstanding == 0);
    const std::uint64_t reads = rig.disk.read_count;
    REQUIRE(ReadSectors(rig.cached, 2, 1, got).has_value());
    REQUIRE(rig.disk.read_count > reads);
    REQUIRE(rig.cache.Statistics().bypasses == 2);

    REQUIRE(ReadSectors(rig.cached, 4, 2, got).error() == Error::Io);
    REQUIRE(rig.cache.Statistics().pages == 0);
    REQUIRE(rig.source.outstanding == 0);

    rig.disk.RestoreReads();
    const std::uint64_t before = rig.disk.read_count;
    REQUIRE(ReadSectors(rig.cached, 4, 2, got).has_value());
    REQUIRE(got == Slice(rig.disk, 4, 2));
    REQUIRE(rig.disk.read_count > before);
    REQUIRE(rig.cache.Statistics().pages == 1);
    const std::uint64_t cached_reads = rig.disk.read_count;
    REQUIRE(ReadSectors(rig.cached, 4, 2, got).has_value());
    REQUIRE(rig.disk.read_count == cached_reads);
}

TEST_CASE("page cache: a page that was evicted is read from the device again", "[page_cache]")
{
    Rig rig;
    std::vector<std::uint8_t> got;
    REQUIRE(ReadSectors(rig.cached, 0, 1, got).has_value());
    REQUIRE(rig.cache.Shrink(1) == 1);
    rig.disk.FailReadsAt(0);
    REQUIRE(ReadSectors(rig.cached, 0, 1, got).error() == Error::Io);
}

TEST_CASE("page cache: invalidating one volume keeps the others", "[page_cache]")
{
    MemoryDevice disk(SectorBytes, 64);
    Fill(disk, 5);
    Volume first_volume(disk, Range{0, 32});
    Volume second_volume(disk, Range{32, 32});
    TestPageSource source;
    PageCache cache;
    REQUIRE(cache.Initialize(source, 8).has_value());
    CachedDevice first;
    CachedDevice second;
    first.Attach(&cache, first_volume);
    second.Attach(&cache, second_volume);

    std::vector<std::uint8_t> got;
    REQUIRE(ReadSectors(first, 0, 8, got).has_value());
    REQUIRE(ReadSectors(second, 0, 8, got).has_value());
    first.Invalidate();
    REQUIRE(cache.Statistics().pages == 1);
    std::uint64_t reads = disk.read_count;
    REQUIRE(ReadSectors(second, 0, 8, got).has_value());
    REQUIRE(disk.read_count == reads);
    REQUIRE(ReadSectors(first, 0, 8, got).has_value());
    REQUIRE(disk.read_count > reads);

    // Changed behind the cache's back, the bytes are seen after Invalidate, and after a detach
    // and an attach.
    disk.Sector(1)[0] ^= 0xFF;
    first.Invalidate();
    REQUIRE(ReadSectors(first, 1, 1, got).has_value());
    REQUIRE(got == Slice(disk, 1, 1));
    disk.Sector(2)[0] ^= 0xFF;
    first.Detach();
    REQUIRE(ReadSectors(first, 2, 1, got).has_value());
    REQUIRE(cache.Statistics().pages == 1);
    first.Attach(&cache, first_volume);
    REQUIRE(ReadSectors(first, 2, 1, got).has_value());
    REQUIRE(got == Slice(disk, 2, 1));
    reads = disk.read_count;
    REQUIRE(ReadSectors(first, 2, 1, got).has_value());
    REQUIRE(disk.read_count == reads);
}

TEST_CASE("page cache: a fill that overlapped a write or an invalidation is not cached",
          "[page_cache]")
{
    Rig rig(4);
    std::vector<std::uint8_t> got;

    const std::vector<std::uint8_t> fresh(SectorBytes, 0xC3);
    rig.disk.on_read = [&](std::uint64_t, std::uint64_t)
    {
        rig.disk.on_read = nullptr;
        REQUIRE(rig.cached.Write(1, fresh).has_value());
    };
    REQUIRE(ReadSectors(rig.cached, 0, 1, got).has_value());
    REQUIRE(rig.cache.Statistics().pages == 0);
    std::uint64_t reads = rig.disk.read_count;
    REQUIRE(ReadSectors(rig.cached, 1, 1, got).has_value());
    REQUIRE(got == fresh);
    REQUIRE(rig.disk.read_count > reads);
    REQUIRE(rig.cache.Statistics().pages == 1);

    rig.disk.on_read = [&](std::uint64_t, std::uint64_t)
    {
        rig.disk.on_read = nullptr;
        rig.disk.Sector(9)[0] ^= 0xFF;
        rig.cached.Invalidate();
    };
    REQUIRE(ReadSectors(rig.cached, 8, 1, got).has_value());
    REQUIRE(rig.cache.Statistics().pages == 0);
    REQUIRE(ReadSectors(rig.cached, 9, 1, got).has_value());
    REQUIRE(got == Slice(rig.disk, 9, 1));

    rig.disk.on_read = [&](std::uint64_t, std::uint64_t)
    {
        rig.disk.on_read = nullptr;
        rig.disk.Sector(17)[0] ^= 0xFF;
        rig.cache.InvalidateAll();
    };
    REQUIRE(ReadSectors(rig.cached, 16, 1, got).has_value());
    REQUIRE(ReadSectors(rig.cached, 17, 1, got).has_value());
    REQUIRE(got == Slice(rig.disk, 17, 1));
    reads = rig.disk.read_count;
    REQUIRE(ReadSectors(rig.cached, 17, 1, got).has_value());
    REQUIRE(rig.disk.read_count == reads);
}

TEST_CASE("page cache: volumes with the same relative page keep their own bytes", "[page_cache]")
{
    MemoryDevice disk(SectorBytes, 48);
    Fill(disk, 6);
    Volume first_volume(disk, Range{0, 16});
    Volume second_volume(disk, Range{16, 16});
    TestPageSource source;
    PageCache cache;
    REQUIRE(cache.Initialize(source, 8).has_value());
    CachedDevice first;
    CachedDevice second;
    first.Attach(&cache, first_volume);
    second.Attach(&cache, second_volume);

    std::vector<std::uint8_t> got;
    REQUIRE(ReadSectors(first, 0, 8, got).has_value());
    REQUIRE(got == Slice(disk, 0, 8));
    REQUIRE(ReadSectors(second, 0, 8, got).has_value());
    REQUIRE(got == Slice(disk, 16, 8));
    REQUIRE(ReadSectors(first, 0, 8, got).has_value());
    REQUIRE(got == Slice(disk, 0, 8));

    // The same slot attached again over another range never sees the earlier pages.
    first.Detach();
    first_volume.Assign(disk, Range{32, 16});
    first.Attach(&cache, first_volume);
    REQUIRE(ReadSectors(first, 0, 8, got).has_value());
    REQUIRE(got == Slice(disk, 32, 8));
    first.Attach(&cache, first_volume);
    REQUIRE(ReadSectors(first, 0, 8, got).has_value());
    REQUIRE(got == Slice(disk, 32, 8));
}

TEST_CASE("page cache: the pages it holds are the source's outstanding pages", "[page_cache]")
{
    TestPageSource source;
    {
        MemoryDevice disk(SectorBytes, 64);
        Fill(disk, 7);
        Volume volume(disk, Range{0, 64});
        PageCache cache;
        REQUIRE(cache.Initialize(source, 4).has_value());
        CachedDevice cached;
        cached.Attach(&cache, volume);
        std::vector<std::uint8_t> got;

        REQUIRE(ReadSectors(cached, 0, 32, got).has_value());
        REQUIRE(cache.Statistics().pages == 4);
        REQUIRE(source.outstanding == 4);
        REQUIRE(cache.Shrink(3) == 3);
        REQUIRE(source.outstanding == 1);
        REQUIRE(cache.Shrink(10) == 1);
        REQUIRE(cache.Shrink(10) == 0);
        REQUIRE(source.outstanding == 0);
        REQUIRE(cache.Statistics().reclaimed == 4);

        // With no headroom a fill recycles a page of its own and allocates nothing.
        REQUIRE(ReadSectors(cached, 0, 16, got).has_value());
        REQUIRE(source.outstanding == 2);
        source.budget = 0;
        const std::size_t allocations = source.allocations;
        REQUIRE(ReadSectors(cached, 32, 16, got).has_value());
        REQUIRE(got == Slice(disk, 32, 16));
        REQUIRE(source.allocations == allocations);
        REQUIRE(source.outstanding == 2);
        REQUIRE(cache.Statistics().evictions == 2);
        REQUIRE(cache.Balance() == 2);
        REQUIRE(source.outstanding == 0);

        // Nothing to recycle: the read goes to the device and caches nothing.
        REQUIRE(ReadSectors(cached, 0, 8, got).has_value());
        REQUIRE(got == Slice(disk, 0, 8));
        REQUIRE(source.outstanding == 0);
        REQUIRE(cache.Statistics().bypasses == 1);

        // A source that cannot allocate costs speed, never an error.
        source.budget = SIZE_MAX;
        source.fail = true;
        REQUIRE(ReadSectors(cached, 8, 8, got).has_value());
        REQUIRE(got == Slice(disk, 8, 8));
        REQUIRE(cache.Statistics().bypasses == 2);
        source.fail = false;

        REQUIRE(ReadSectors(cached, 0, 32, got).has_value());
        REQUIRE(source.outstanding == 4);
        REQUIRE(cache.Statistics().pages <= cache.Statistics().limit);
        cache.InvalidateAll();
        REQUIRE(source.outstanding == 0);
        REQUIRE(ReadSectors(cached, 0, 16, got).has_value());
        REQUIRE(source.outstanding == 2);
    }
    REQUIRE(source.outstanding == 0);
}

TEST_CASE("page cache: balance shrinks until the source has headroom", "[page_cache]")
{
    Rig rig(8);
    std::vector<std::uint8_t> got;
    REQUIRE(ReadSectors(rig.cached, 0, 40, got).has_value());
    REQUIRE(rig.source.outstanding == 5);
    REQUIRE(rig.cache.Balance() == 0);
    rig.source.budget = 3;
    REQUIRE(rig.cache.Balance() == 3);
    REQUIRE(rig.source.outstanding == 2);
    REQUIRE(rig.cache.Statistics().reclaimed == 3);
}

TEST_CASE("page cache: sizes it cannot page pass through", "[page_cache]")
{
    SECTION("sectors of a whole page")
    {
        Rig rig(4, 16, 4096);
        std::vector<std::uint8_t> got;
        REQUIRE(ReadSectors(rig.cached, 2, 3, got).has_value());
        REQUIRE(rig.cache.Statistics().pages == 3);
        const std::uint64_t reads = rig.disk.read_count;
        REQUIRE(ReadSectors(rig.cached, 2, 3, got).has_value());
        REQUIRE(got == Slice(rig.disk, 2, 3));
        REQUIRE(rig.disk.read_count == reads);
    }
    SECTION("sectors over a page")
    {
        Rig rig(4, 8, 8192);
        std::vector<std::uint8_t> got;
        REQUIRE(ReadSectors(rig.cached, 1, 2, got).has_value());
        REQUIRE(got == Slice(rig.disk, 1, 2));
        REQUIRE(ReadSectors(rig.cached, 1, 2, got).has_value());
        REQUIRE(rig.cache.Statistics().pages == 0);
        REQUIRE(rig.disk.read_count == 2);
    }
    SECTION("no cache")
    {
        MemoryDevice disk(SectorBytes, 16);
        Fill(disk, 8);
        CachedDevice cached;
        std::vector<std::uint8_t> got;
        REQUIRE(ReadSectors(cached, 0, 1, got).error() == Error::InvalidArgument);
        cached.Attach(nullptr, disk);
        REQUIRE(ReadSectors(cached, 0, 4, got).has_value());
        REQUIRE(got == Slice(disk, 0, 4));
        REQUIRE(ReadSectors(cached, 0, 4, got).has_value());
        REQUIRE(disk.read_count == 2);
        REQUIRE(cached.Flush().has_value());
        REQUIRE(disk.flush_count == 1);
    }
}

TEST_CASE("page cache: several volumes and a shrinking thread stay correct",
          "[page_cache][concurrency]")
{
    constexpr std::size_t Threads = 4;
    constexpr std::uint64_t VolumeSectors = 29;
    MemoryDevice disk(SectorBytes, Threads * 32);
    Fill(disk, 9);
    LockedDevice locked(disk);
    TestPageSource source;
    std::atomic<bool> done{false};
    std::array<bool, Threads> correct{};
    {
        PageCache cache;
        REQUIRE(cache.Initialize(source, 6).has_value());
        std::array<Volume, Threads> volumes;
        std::array<CachedDevice, Threads> devices;
        for (std::size_t index = 0; index < Threads; ++index)
        {
            volumes[index].Assign(locked, Range{index * 32, VolumeSectors});
            devices[index].Attach(&cache, volumes[index]);
        }

        std::thread shrinker(
            [&cache, &done]
            {
                while (!done)
                {
                    cache.Shrink(2);
                    cache.Balance();
                    static_cast<void>(cache.Statistics());
                }
            });
        std::vector<std::thread> workers;
        for (std::size_t index = 0; index < Threads; ++index)
        {
            workers.emplace_back(
                [&, index]
                {
                    std::vector<std::uint8_t> model = Slice(disk, index * 32, VolumeSectors);
                    std::mt19937 random(100 + static_cast<unsigned>(index));
                    std::vector<std::uint8_t> got;
                    bool all = true;
                    for (unsigned step = 0; step < 1500; ++step)
                    {
                        const std::uint64_t first = random() % VolumeSectors;
                        const std::uint64_t count =
                            1 + random() % std::min<std::uint64_t>(12, VolumeSectors - first);
                        const unsigned action = random() % 100;
                        if (action < 60)
                        {
                            all = all &&
                                  ReadSectors(devices[index], first, count, got).has_value() &&
                                  std::equal(got.begin(), got.end(),
                                             model.begin() + first * SectorBytes);
                        }
                        else if (action < 90)
                        {
                            std::vector<std::uint8_t> bytes(count * SectorBytes);
                            for (std::uint8_t& byte : bytes)
                            {
                                byte = static_cast<std::uint8_t>(random());
                            }
                            all = all && devices[index].Write(first, bytes).has_value();
                            std::ranges::copy(bytes, model.begin() + first * SectorBytes);
                        }
                        else if (action < 97)
                        {
                            devices[index].Invalidate();
                        }
                        else
                        {
                            devices[index].Detach();
                            devices[index].Attach(&cache, volumes[index]);
                        }
                    }
                    correct[index] = all;
                });
        }
        for (std::thread& worker : workers)
        {
            worker.join();
        }
        done = true;
        shrinker.join();
        cache.InvalidateAll();
        REQUIRE(source.outstanding == 0);
    }
    REQUIRE(correct == std::array<bool, Threads>{true, true, true, true});
}
