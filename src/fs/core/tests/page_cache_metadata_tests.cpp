// SPDX-License-Identifier: MIT OR Apache-2.0
// The core's metadata path over a caching device: the block cache keeps nothing and the page cache
// is the only copy of a disk block below the core.
#include <catch2/catch_test_macros.hpp>

#include "ext/ext2_filesystem.h"
#include "fs/ext2.h"
#include "fs/page_cache.h"
#include "tests/crash_helpers.h"
#include "tests/crash_scenarios.h"
#include "tests/ext2_image.h"
#include "tests/filesystem_helpers.h"
#include "tests/memory_device.h"
#include "tests/page_cache_support.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>

namespace
{

using namespace FeatherDisk;
using namespace FeatherDisk::Tests;
using namespace FeatherDisk::Tests::Crash;
using Ext::Ext2Type;

constexpr std::size_t PlainCacheBytes = 32 * 1024;

TEST_CASE("a cached device reports that it caches only while it is attached")
{
    MemoryDevice disk(512, 64);
    TestPageSource source;
    PageCache cache;
    REQUIRE(cache.Initialize(source, 4).has_value());
    CachedDevice cached;
    REQUIRE_FALSE(disk.CachesBlocks());
    REQUIRE_FALSE(cached.CachesBlocks());
    cached.Attach(&cache, disk);
    REQUIRE(cached.CachesBlocks());
    cached.Invalidate();
    REQUIRE(cached.CachesBlocks());
    cached.Detach();
    REQUIRE_FALSE(cached.CachesBlocks());
    cached.Attach(nullptr, disk);
    REQUIRE_FALSE(cached.CachesBlocks());
    MemoryDevice large_sectors(8192, 8);
    cached.Attach(&cache, large_sectors);
    REQUIRE_FALSE(cached.CachesBlocks());
}

} // namespace

#ifdef FEATHERDISK_EXT_IMAGES
namespace
{

TEST_CASE("an ext mount keeps no block copies over a device that caches blocks")
{
    for (const std::string_view variant : {"1k", "ext3", "ext4", "4k"})
    {
        INFO(variant);
        const std::unique_ptr<MemoryDevice> disk = DeviceFrom(LoadExtImage(variant));
        const std::size_t block_size = BlockSizeOf(*disk);
        TestPageSource source;
        PageCache cache;
        REQUIRE(cache.Initialize(source, 16).has_value());
        CachedDevice cached;

        cached.Attach(&cache, *disk);
        {
            MountedFilesystem mounted(Ext2Type(), cached, true);
            REQUIRE(Core(*mounted).MetadataCacheSlots() == 0);
        }
        cached.Detach();
        {
            MountedFilesystem mounted(Ext2Type(), cached, true);
            REQUIRE(Core(*mounted).MetadataCacheSlots() * block_size == PlainCacheBytes);
        }
        {
            MountedFilesystem mounted(Ext2Type(), *disk, true);
            REQUIRE(Core(*mounted).MetadataCacheSlots() * block_size == PlainCacheBytes);
        }
    }
}

// Metadata read again and again: the core asks its device each time, and the device below the
// page cache is asked once.
void RepeatedReads(std::string_view variant, bool caching)
{
    INFO(variant << (caching ? " over the page cache" : " over a plain device"));
    const std::unique_ptr<MemoryDevice> disk = DeviceFrom(LoadExtImage(variant));
    TestPageSource source;
    PageCache cache;
    REQUIRE(cache.Initialize(source, 64).has_value());
    CachedDevice cached;
    cached.Attach(caching ? &cache : nullptr, *disk);
    RecordingDevice counting(cached);
    MountedFilesystem mounted(Ext2Type(), counting, true);
    Filesystem& filesystem = *mounted;

    const NodeId root = filesystem.Root();
    const NodeId hello = Value(filesystem.Lookup(root, "hello.txt"));
    Value(filesystem.GetAttributes(root));
    Value(filesystem.GetAttributes(hello));
    const std::size_t counted = counting.reads.size();
    const std::uint64_t lower = disk->read_count;
    REQUIRE(counted > 0);

    std::size_t seen = counted;
    for (int round = 0; round < 3; ++round)
    {
        Value(filesystem.GetAttributes(root));
        if (caching)
        {
            REQUIRE(counting.reads.size() > seen);
        }
        seen = counting.reads.size();
        REQUIRE(Value(filesystem.Lookup(root, "hello.txt")) == hello);
        if (caching)
        {
            REQUIRE(counting.reads.size() > seen);
        }
        seen = counting.reads.size();
        Value(filesystem.GetAttributes(hello));
        if (caching)
        {
            REQUIRE(counting.reads.size() > seen);
        }
        seen = counting.reads.size();
    }
    if (!caching)
    {
        REQUIRE(counting.reads.size() == counted);
    }
    REQUIRE(disk->read_count == lower);
}

TEST_CASE("repeated metadata reads reach the device above the page cache and stop below it")
{
    for (const std::string_view variant : {"ext4", "1k", "ext3"})
    {
        RepeatedReads(variant, true);
        RepeatedReads(variant, false);
    }
}

void RequireSameImages(std::string_view variant, std::uint32_t seed, std::size_t count)
{
    INFO(variant << " seed " << seed);
    const FrozenClock frozen;
    const std::unique_ptr<MemoryDevice> base = Prepared(variant);
    const Scenario scenario = RandomScenario(seed * 7919, count, false);
    const Reference today = RunReference(*base, scenario, std::nullopt, false, false, 0);
    const Reference cached = RunReference(*base, scenario, std::nullopt, false, false, 8);
    REQUIRE(today.total == cached.total);
    REQUIRE(today.flushes == cached.flushes);
    REQUIRE(today.final_device->call_digest == cached.final_device->call_digest);
    REQUIRE(std::memcmp(today.final_device->Bytes().data(), cached.final_device->Bytes().data(),
                        today.final_device->Bytes().size()) == 0);
    // RunReference ran e2fsck -fn over both.
}

TEST_CASE("random changes through the page cache leave a clean volume equal to the uncached run")
{
    for (const std::string_view variant : {"ext4", "1k"})
    {
        for (std::uint32_t seed = 1; seed <= 3; ++seed)
        {
            RequireSameImages(variant, seed, 200);
        }
    }
}

} // namespace
#endif
