// SPDX-License-Identifier: MIT OR Apache-2.0
#include "fs/page_cache.h"

#include "common/bytes.h"

#include <array>

namespace FeatherDisk
{

namespace
{

// Pages freed or scanned per hold of the lock, which bounds how long a reader can be kept waiting.
constexpr std::size_t FreeBatch = 32;
constexpr std::size_t ScanSpan = 512;

constexpr std::uint64_t Smaller(std::uint64_t left, std::uint64_t right)
{
    return left < right ? left : right;
}

constexpr std::uint64_t Larger(std::uint64_t left, std::uint64_t right)
{
    return left < right ? right : left;
}

} // namespace

struct PageCache::Entry
{
    std::uint64_t volume;
    std::uint64_t page;
    // Null for an entry on the free list.
    std::uint8_t* data;
    std::uint32_t lru_prev;
    std::uint32_t lru_next;
    // The next entry of the bucket's chain, or of the free list.
    std::uint32_t hash_next;
};

struct PageCache::Geometry
{
    std::uint32_t sector_size;
    std::uint32_t sectors_per_page;
    std::uint64_t sector_count;
};

PageCache::~PageCache()
{
    if (lock == nullptr)
    {
        return;
    }
    for (std::size_t index = 0; index < limit; ++index)
    {
        if (entries[index].data != nullptr)
        {
            source->FreePage(entries[index].data);
        }
    }
    Host::Free(entries);
    Host::Free(buckets);
    Host::DestroyLock(lock);
}

Status PageCache::Initialize(PageSource& page_source, std::size_t page_limit)
{
    if (lock != nullptr || page_limit >= None)
    {
        return std::unexpected(Error::InvalidArgument);
    }
    std::size_t bucket_count = 1;
    while (bucket_count < page_limit)
    {
        bucket_count *= 2;
    }
    auto* new_entries =
        static_cast<Entry*>(Host::Allocate((page_limit == 0 ? 1 : page_limit) * sizeof(Entry)));
    auto* new_buckets =
        static_cast<std::uint32_t*>(Host::Allocate(bucket_count * sizeof(std::uint32_t)));
    Host::Lock* new_lock = Host::CreateLock();
    if (new_entries == nullptr || new_buckets == nullptr || new_lock == nullptr)
    {
        Host::Free(new_entries);
        Host::Free(new_buckets);
        if (new_lock != nullptr)
        {
            Host::DestroyLock(new_lock);
        }
        return std::unexpected(Error::OutOfMemory);
    }
    for (std::size_t index = 0; index < bucket_count; ++index)
    {
        new_buckets[index] = None;
    }
    for (std::size_t index = 0; index < page_limit; ++index)
    {
        new_entries[index] = {
            .volume = 0,
            .page = 0,
            .data = nullptr,
            .lru_prev = None,
            .lru_next = None,
            .hash_next = index + 1 < page_limit ? static_cast<std::uint32_t>(index + 1) : None};
    }
    source = &page_source;
    entries = new_entries;
    buckets = new_buckets;
    bucket_mask = bucket_count - 1;
    limit = page_limit;
    free_head = page_limit == 0 ? None : 0;
    lock = new_lock;
    return {};
}

std::uint32_t& PageCache::Bucket(std::uint64_t volume, std::uint64_t page) const
{
    std::uint64_t hash = (volume * 0x9E3779B97F4A7C15ULL) ^ page;
    hash *= 0xBF58476D1CE4E5B9ULL;
    hash ^= hash >> 31;
    return buckets[hash & bucket_mask];
}

std::uint32_t PageCache::Find(std::uint64_t volume, std::uint64_t page) const
{
    for (std::uint32_t index = Bucket(volume, page); index != None;
         index = entries[index].hash_next)
    {
        if (entries[index].volume == volume && entries[index].page == page)
        {
            return index;
        }
    }
    return None;
}

void PageCache::LruRemove(std::uint32_t index)
{
    const Entry& entry = entries[index];
    if (entry.lru_prev != None)
    {
        entries[entry.lru_prev].lru_next = entry.lru_next;
    }
    else
    {
        lru_head = entry.lru_next;
    }
    if (entry.lru_next != None)
    {
        entries[entry.lru_next].lru_prev = entry.lru_prev;
    }
    else
    {
        lru_tail = entry.lru_prev;
    }
}

void PageCache::LruPush(std::uint32_t index)
{
    Entry& entry = entries[index];
    entry.lru_prev = None;
    entry.lru_next = lru_head;
    if (lru_head != None)
    {
        entries[lru_head].lru_prev = index;
    }
    else
    {
        lru_tail = index;
    }
    lru_head = index;
}

std::uint8_t* PageCache::Unlink(std::uint32_t index)
{
    Entry& entry = entries[index];
    std::uint32_t* link = &Bucket(entry.volume, entry.page);
    while (*link != index)
    {
        link = &entries[*link].hash_next;
    }
    *link = entry.hash_next;
    LruRemove(index);
    std::uint8_t* const data = entry.data;
    entry.data = nullptr;
    entry.hash_next = free_head;
    free_head = index;
    --pages;
    return data;
}

std::uint8_t* PageCache::Insert(std::uint64_t volume, std::uint64_t page, std::uint8_t* data)
{
    std::uint8_t* evicted = nullptr;
    if (free_head == None)
    {
        evicted = Unlink(lru_tail);
        ++counters.evictions;
    }
    const std::uint32_t index = free_head;
    Entry& entry = entries[index];
    free_head = entry.hash_next;
    std::uint32_t& chain = Bucket(volume, page);
    entry.volume = volume;
    entry.page = page;
    entry.data = data;
    entry.hash_next = chain;
    chain = index;
    LruPush(index);
    ++pages;
    return evicted;
}

std::uint64_t PageCache::NewId()
{
    Host::Acquire(*lock);
    const std::uint64_t id = ++next_id;
    Host::Release(*lock);
    return id;
}

void PageCache::BumpEpoch(CachedDevice& device)
{
    Host::Acquire(*lock);
    ++device.epoch;
    Host::Release(*lock);
}

void PageCache::CountBypass()
{
    Host::Acquire(*lock);
    ++counters.bypasses;
    Host::Release(*lock);
}

std::uint8_t* PageCache::TakeVictimFrame()
{
    Host::Acquire(*lock);
    std::uint8_t* frame = nullptr;
    if (lru_tail != None)
    {
        frame = Unlink(lru_tail);
        ++counters.evictions;
    }
    Host::Release(*lock);
    return frame;
}

Status PageCache::ReadPage(CachedDevice& device, const Geometry& geometry, std::uint64_t page,
                           std::uint32_t first_sector, std::span<std::uint8_t> out)
{
    const std::size_t offset = static_cast<std::size_t>(first_sector) * geometry.sector_size;
    Host::Acquire(*lock);
    if (const std::uint32_t found = Find(device.id, page); found != None)
    {
        CopyBytes(out.data(), entries[found].data + offset, out.size());
        if (lru_head != found)
        {
            LruRemove(found);
            LruPush(found);
        }
        ++counters.hits;
        Host::Release(*lock);
        return {};
    }
    ++counters.misses;
    const std::uint64_t epoch = device.epoch;
    const std::uint64_t era = generation;
    const bool may_grow = pages < limit;
    Host::Release(*lock);

    std::uint8_t* frame = nullptr;
    if (may_grow && source->Headroom() > 0)
    {
        frame = source->AllocatePage();
    }
    if (frame == nullptr)
    {
        frame = TakeVictimFrame();
    }
    const std::uint64_t page_first = page * geometry.sectors_per_page;
    if (frame == nullptr)
    {
        CountBypass();
        return device.lower->Read(page_first + first_sector, out);
    }

    const auto sectors = static_cast<std::uint32_t>(
        Smaller(geometry.sectors_per_page, geometry.sector_count - page_first));
    if (const Status filled = device.lower->Read(
            page_first, std::span(frame, static_cast<std::size_t>(sectors) * geometry.sector_size));
        !filled)
    {
        source->FreePage(frame);
        CountBypass();
        return device.lower->Read(page_first + first_sector, out);
    }
    CopyBytes(out.data(), frame + offset, out.size());

    std::uint8_t* spare = frame;
    std::uint8_t* evicted = nullptr;
    Host::Acquire(*lock);
    if (device.epoch == epoch && generation == era && Find(device.id, page) == None)
    {
        evicted = Insert(device.id, page, frame);
        spare = nullptr;
    }
    Host::Release(*lock);
    if (spare != nullptr)
    {
        source->FreePage(spare);
    }
    if (evicted != nullptr)
    {
        source->FreePage(evicted);
    }
    return {};
}

Status PageCache::Read(CachedDevice& device, std::uint64_t first_sector,
                       std::span<std::uint8_t> buffer)
{
    BlockDevice& lower = *device.lower;
    const std::uint32_t sector_size = lower.SectorSize();
    const Geometry geometry = {sector_size, static_cast<std::uint32_t>(CachePageSize / sector_size),
                               lower.SectorCount()};
    const std::uint64_t count = buffer.size() / sector_size;
    if (buffer.empty() || buffer.size() % sector_size != 0 ||
        first_sector > geometry.sector_count || count > geometry.sector_count - first_sector)
    {
        return lower.Read(first_sector, buffer);
    }
    const std::uint64_t end = first_sector + count;
    for (std::uint64_t page = first_sector / geometry.sectors_per_page;
         page * geometry.sectors_per_page < end; ++page)
    {
        const std::uint64_t low = Larger(first_sector, page * geometry.sectors_per_page);
        const std::uint64_t high = Smaller(end, (page + 1) * geometry.sectors_per_page);
        const Status read = ReadPage(
            device, geometry, page,
            static_cast<std::uint32_t>(low - page * geometry.sectors_per_page),
            buffer.subspan((low - first_sector) * sector_size, (high - low) * sector_size));
        if (!read)
        {
            return read;
        }
    }
    return {};
}

void PageCache::UpdatePage(const CachedDevice& device, const Geometry& geometry, std::uint64_t page,
                           std::uint32_t first_sector, std::span<const std::uint8_t> bytes,
                           bool written)
{
    std::uint8_t* dropped = nullptr;
    Host::Acquire(*lock);
    if (const std::uint32_t found = Find(device.id, page); found != None)
    {
        if (written)
        {
            CopyBytes(entries[found].data +
                          static_cast<std::size_t>(first_sector) * geometry.sector_size,
                      bytes.data(), bytes.size());
        }
        else
        {
            dropped = Unlink(found);
            ++counters.invalidated;
        }
    }
    Host::Release(*lock);
    if (dropped != nullptr)
    {
        source->FreePage(dropped);
    }
}

Status PageCache::Write(CachedDevice& device, std::uint64_t first_sector,
                        std::span<const std::uint8_t> data)
{
    BlockDevice& lower = *device.lower;
    const std::uint32_t sector_size = lower.SectorSize();
    const Geometry geometry = {sector_size, static_cast<std::uint32_t>(CachePageSize / sector_size),
                               lower.SectorCount()};
    const std::uint64_t count = data.size() / sector_size;
    if (data.empty() || data.size() % sector_size != 0 || first_sector > geometry.sector_count ||
        count > geometry.sector_count - first_sector)
    {
        return lower.Write(first_sector, data);
    }

    BumpEpoch(device);
    const Status written = lower.Write(first_sector, data);
    BumpEpoch(device);

    const std::uint64_t end = first_sector + count;
    for (std::uint64_t page = first_sector / geometry.sectors_per_page;
         page * geometry.sectors_per_page < end; ++page)
    {
        const std::uint64_t low = Larger(first_sector, page * geometry.sectors_per_page);
        const std::uint64_t high = Smaller(end, (page + 1) * geometry.sectors_per_page);
        UpdatePage(device, geometry, page,
                   static_cast<std::uint32_t>(low - page * geometry.sectors_per_page),
                   data.subspan((low - first_sector) * sector_size, (high - low) * sector_size),
                   written.has_value());
    }
    return written;
}

void PageCache::Drop(CachedDevice* device)
{
    Host::Acquire(*lock);
    if (device != nullptr)
    {
        ++device->epoch;
    }
    else
    {
        ++generation;
    }
    Host::Release(*lock);

    std::size_t cursor = 0;
    while (cursor < limit)
    {
        std::array<std::uint8_t*, FreeBatch> frames{};
        std::size_t dropped = 0;
        std::size_t scanned = 0;
        Host::Acquire(*lock);
        while (cursor < limit && dropped < FreeBatch && scanned < ScanSpan)
        {
            const Entry& entry = entries[cursor];
            if (entry.data != nullptr && (device == nullptr || entry.volume == device->id))
            {
                frames[dropped++] = Unlink(static_cast<std::uint32_t>(cursor));
            }
            ++cursor;
            ++scanned;
        }
        counters.invalidated += dropped;
        Host::Release(*lock);
        for (std::size_t index = 0; index < dropped; ++index)
        {
            source->FreePage(frames[index]);
        }
    }
}

void PageCache::InvalidateAll()
{
    if (lock != nullptr)
    {
        Drop(nullptr);
    }
}

std::size_t PageCache::Shrink(std::size_t wanted)
{
    if (lock == nullptr)
    {
        return 0;
    }
    std::size_t total = 0;
    while (total < wanted)
    {
        std::array<std::uint8_t*, FreeBatch> frames{};
        std::size_t freed = 0;
        Host::Acquire(*lock);
        while (freed < FreeBatch && total + freed < wanted && lru_tail != None)
        {
            frames[freed++] = Unlink(lru_tail);
        }
        counters.reclaimed += freed;
        Host::Release(*lock);
        for (std::size_t index = 0; index < freed; ++index)
        {
            source->FreePage(frames[index]);
        }
        if (freed == 0)
        {
            break;
        }
        total += freed;
    }
    return total;
}

std::size_t PageCache::Balance()
{
    if (lock == nullptr)
    {
        return 0;
    }
    std::size_t total = 0;
    while (source->Headroom() == 0)
    {
        const std::size_t freed = Shrink(1);
        if (freed == 0)
        {
            break;
        }
        total += freed;
    }
    return total;
}

PageCacheStatistics PageCache::Statistics() const
{
    if (lock == nullptr)
    {
        return {};
    }
    Host::Acquire(*lock);
    PageCacheStatistics result = counters;
    result.pages = pages;
    result.limit = limit;
    Host::Release(*lock);
    return result;
}

void CachedDevice::Attach(PageCache* page_cache, BlockDevice& lower_device)
{
    Detach();
    lower = &lower_device;
    const std::uint32_t sector_size = lower_device.SectorSize();
    if (page_cache != nullptr && page_cache->lock != nullptr && sector_size != 0 &&
        CachePageSize % sector_size == 0)
    {
        cache = page_cache;
        id = cache->NewId();
    }
}

void CachedDevice::Invalidate()
{
    if (cache != nullptr)
    {
        cache->Drop(this);
    }
}

void CachedDevice::Detach()
{
    Invalidate();
    cache = nullptr;
}

bool CachedDevice::CachesBlocks() const
{
    return cache != nullptr;
}

std::uint32_t CachedDevice::SectorSize() const
{
    return lower == nullptr ? 512 : lower->SectorSize();
}

std::uint64_t CachedDevice::SectorCount() const
{
    return lower == nullptr ? 0 : lower->SectorCount();
}

Status CachedDevice::Read(std::uint64_t first_sector, std::span<std::uint8_t> buffer)
{
    if (lower == nullptr)
    {
        return std::unexpected(Error::InvalidArgument);
    }
    return cache == nullptr ? lower->Read(first_sector, buffer)
                            : cache->Read(*this, first_sector, buffer);
}

Status CachedDevice::Write(std::uint64_t first_sector, std::span<const std::uint8_t> data)
{
    if (lower == nullptr)
    {
        return std::unexpected(Error::InvalidArgument);
    }
    return cache == nullptr ? lower->Write(first_sector, data)
                            : cache->Write(*this, first_sector, data);
}

Status CachedDevice::Flush()
{
    if (lower == nullptr)
    {
        return std::unexpected(Error::InvalidArgument);
    }
    return lower->Flush();
}

} // namespace FeatherDisk
