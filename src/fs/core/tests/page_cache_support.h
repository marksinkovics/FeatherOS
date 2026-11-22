// SPDX-License-Identifier: MIT OR Apache-2.0
// Page-cache parts the tests share: a heap page source, a device that records its calls, and the
// device a filesystem is mounted over.
#ifndef FEATHERDISK_TESTS_PAGE_CACHE_SUPPORT_H
#define FEATHERDISK_TESTS_PAGE_CACHE_SUPPORT_H

#include "fs/block_device.h"
#include "fs/page_cache.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace FeatherDisk::Tests
{

class TestPageSource final : public PageSource
{
public:
    std::uint8_t* AllocatePage() override;
    void FreePage(std::uint8_t* page) override;
    std::size_t Headroom() const override;

    std::atomic<std::size_t> outstanding{0};
    std::atomic<std::size_t> allocations{0};
    std::atomic<std::size_t> budget{SIZE_MAX};
    std::atomic<bool> fail{false};
};

// Passes everything down, including whether the device below caches.
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

    struct ReadCall
    {
        std::uint64_t first_sector;
        std::uint64_t sector_count;
        bool operator==(const ReadCall&) const = default;
    };

    explicit RecordingDevice(BlockDevice& below);

    std::uint32_t SectorSize() const override;
    std::uint64_t SectorCount() const override;
    Status Read(std::uint64_t first_sector, std::span<std::uint8_t> buffer) override;
    Status Write(std::uint64_t first_sector, std::span<const std::uint8_t> data) override;
    Status Flush() override;
    bool CachesBlocks() const override;

    // Writes and flushes, and reads, in order.
    std::vector<Call> calls;
    std::vector<ReadCall> reads;
    bool fail_writes = false;

private:
    BlockDevice& lower;
};

// Wall-clock time stands still while it lives, so that two runs write the same bytes.
class FrozenClock
{
public:
    FrozenClock();
    ~FrozenClock();
    FrozenClock(const FrozenClock&) = delete;
    FrozenClock& operator=(const FrozenClock&) = delete;
};

// What a filesystem under test is mounted over: the device itself, or, in a build with
// FEATHERDISK_TEST_PAGE_CACHE, a CachedDevice over it, attached anew on every call as the kernel
// does at every mount, to one small cache all the mounts share.
BlockDevice& Mountable(BlockDevice& device);

} // namespace FeatherDisk::Tests

#endif
