#include "page_cache.h"

#include "cache_sizing.h"

#include "kernel/memory/page_allocator.h"

#include <new>

namespace FeatherOS::BlockDevice
{
namespace
{

class FramePages final : public FeatherDisk::PageSource
{
public:
    constexpr FramePages() = default;

    void Configure(uint64_t hhdm, uint64_t low_watermark)
    {
        hhdm_offset = hhdm;
        watermark = low_watermark;
    }

    uint8_t* AllocatePage() override
    {
        uint64_t physical;
        if (!Memory::AllocatePageFrame(&physical))
        {
            return nullptr;
        }
        return reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(hhdm_offset + physical));
    }

    void FreePage(uint8_t* page) override
    {
        Memory::FreePageFrame(reinterpret_cast<uintptr_t>(page) - hhdm_offset);
    }

    size_t Headroom() const override
    {
        const uint64_t free = Memory::FreeFrameCount();
        return free > watermark ? static_cast<size_t>(free - watermark) : 0;
    }

private:
    uint64_t hhdm_offset = 0;
    uint64_t watermark = 0;
};

constinit FramePages frames;
constinit uint64_t trim_margin = 0;

// The kernel runs no static destructors, which PageCache has, so it lives in raw storage that
// InitializeCache constructs.
alignas(FeatherDisk::PageCache) uint8_t cache_storage[sizeof(FeatherDisk::PageCache)];

FeatherDisk::PageCache& CacheInstance()
{
    return *std::launder(reinterpret_cast<FeatherDisk::PageCache*>(cache_storage));
}

} // namespace

uint64_t ReclaimFrames(uint64_t wanted)
{
    return CacheInstance().Shrink(static_cast<size_t>(wanted));
}

bool InitializeCache(uint64_t hhdm_offset)
{
    Memory::PageAllocatorStats stats = {};
    if (!Memory::GetPageAllocatorStats(&stats))
    {
        return false;
    }
    frames.Configure(hhdm_offset, Watermark(stats.total_frames));
    trim_margin = TrimMargin(stats.total_frames);
    auto* created = new (cache_storage) FeatherDisk::PageCache;
    if (!created->Initialize(
            frames, static_cast<size_t>(CacheLimit(stats.free_frames, stats.total_frames))))
    {
        return false;
    }
    Memory::SetFrameReclaimer(ReclaimFrames);
    return true;
}

FeatherDisk::PageCache& Cache()
{
    return CacheInstance();
}

void BalanceCache()
{
    TrimToMargin(CacheInstance(), frames, trim_margin);
}

uint64_t CachedBytes()
{
    return CacheInstance().Statistics().pages * FeatherDisk::CachePageSize;
}

} // namespace FeatherOS::BlockDevice
