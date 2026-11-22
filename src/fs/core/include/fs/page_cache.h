// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_PAGE_CACHE_H
#define FEATHERDISK_PAGE_CACHE_H

#include "fs/block_device.h"
#include "fs/error.h"
#include "fs/host.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace FeatherDisk
{

inline constexpr std::size_t CachePageSize = 4096;

// Where the cache's pages come from. The host owns the memory policy: a kernel hands out frames it
// can take back, which a heap could not.
class PageSource
{
public:
    PageSource() = default;
    PageSource(const PageSource&) = delete;
    PageSource& operator=(const PageSource&) = delete;

    // CachePageSize bytes, or null.
    virtual std::uint8_t* AllocatePage() = 0;
    // Any context; never called with the cache's lock held.
    virtual void FreePage(std::uint8_t* page) = 0;
    // How many pages the cache may still add; called without the cache's lock.
    virtual std::size_t Headroom() const = 0;

protected:
    ~PageSource() = default;
};

struct PageCacheStatistics
{
    std::uint64_t pages;
    std::uint64_t limit;
    std::uint64_t hits;
    std::uint64_t misses;
    // Reads that went to the device without being cached: no frame, or a page that would not fill.
    std::uint64_t bypasses;
    // Pages recycled to make room for a fill.
    std::uint64_t evictions;
    // Pages freed by Shrink and Balance.
    std::uint64_t reclaimed;
    // Pages dropped because their volume was invalidated or a write to them failed.
    std::uint64_t invalidated;
};

class CachedDevice;

// The 4 KiB pages of many volumes, least recently used first out. The host lock covers index, list
// and counter work and copies of one page; a device call, an allocation and a free all happen
// outside it, so every method may run in any context that does not hold the lock itself.
class PageCache
{
public:
    constexpr PageCache() = default;
    ~PageCache();
    PageCache(const PageCache&) = delete;
    PageCache& operator=(const PageCache&) = delete;

    // Allocates the tables, once; page frames come from source, which must outlive the cache.
    Status Initialize(PageSource& page_source, std::size_t page_limit);
    // Frees up to pages least recently used pages and returns how many it freed.
    std::size_t Shrink(std::size_t pages);
    // Shrinks while the source has no headroom; returns the pages freed.
    std::size_t Balance();
    void InvalidateAll();
    PageCacheStatistics Statistics() const;

private:
    friend class CachedDevice;

    struct Entry;
    struct Geometry;

    static constexpr std::uint32_t None = UINT32_MAX;

    std::uint64_t NewId();
    Status Read(CachedDevice& device, std::uint64_t first_sector, std::span<std::uint8_t> buffer);
    Status Write(CachedDevice& device, std::uint64_t first_sector,
                 std::span<const std::uint8_t> data);
    void Drop(CachedDevice* device);

    Status ReadPage(CachedDevice& device, const Geometry& geometry, std::uint64_t page,
                    std::uint32_t first_sector, std::span<std::uint8_t> out);
    void UpdatePage(const CachedDevice& device, const Geometry& geometry, std::uint64_t page,
                    std::uint32_t first_sector, std::span<const std::uint8_t> bytes, bool written);
    std::uint8_t* TakeVictimFrame();
    void CountBypass();
    void BumpEpoch(CachedDevice& device);

    // Under the lock.
    std::uint32_t Find(std::uint64_t volume, std::uint64_t page) const;
    std::uint32_t& Bucket(std::uint64_t volume, std::uint64_t page) const;
    std::uint8_t* Unlink(std::uint32_t index);
    std::uint8_t* Insert(std::uint64_t volume, std::uint64_t page, std::uint8_t* data);
    void LruRemove(std::uint32_t index);
    void LruPush(std::uint32_t index);

    PageSource* source = nullptr;
    Host::Lock* lock = nullptr;
    Entry* entries = nullptr;
    std::uint32_t* buckets = nullptr;
    std::size_t bucket_mask = 0;
    std::size_t limit = 0;
    std::size_t pages = 0;
    std::uint32_t free_head = None;
    std::uint32_t lru_head = None;
    std::uint32_t lru_tail = None;
    std::uint64_t next_id = 0;
    std::uint64_t generation = 0;
    PageCacheStatistics counters = {};
};

// A device whose reads of a volume come from a PageCache when they can. Writes and flushes go
// straight down and the cache never writes on its own, so the device below sees exactly the calls
// made here, in order. Writers to the same sectors must be serialised by the caller.
class CachedDevice final : public BlockDevice
{
public:
    constexpr CachedDevice() = default;

    // Only while no filesystem uses it. A null cache, or a sector size that does not divide
    // CachePageSize, passes everything through. Attaching again drops the old pages first.
    void Attach(PageCache* page_cache, BlockDevice& lower_device);
    // Drops this volume's pages and stays attached.
    void Invalidate();
    // Invalidate, then passes through.
    void Detach();

    std::uint32_t SectorSize() const override;
    std::uint64_t SectorCount() const override;
    Status Read(std::uint64_t first_sector, std::span<std::uint8_t> buffer) override;
    Status Write(std::uint64_t first_sector, std::span<const std::uint8_t> data) override;
    Status Flush() override;
    bool CachesBlocks() const override;

private:
    friend class PageCache;

    PageCache* cache = nullptr;
    BlockDevice* lower = nullptr;
    std::uint64_t id = 0;
    // Guarded by the cache's lock.
    std::uint64_t epoch = 0;
};

} // namespace FeatherDisk

#endif
