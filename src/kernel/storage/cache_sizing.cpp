#include "cache_sizing.h"

#include <algorithm>

namespace FeatherOS::BlockDevice
{

uint64_t Watermark(uint64_t total_frames)
{
    return std::max<uint64_t>(total_frames / 32, MinimumWatermark);
}

uint64_t CacheLimit(uint64_t free_frames, uint64_t total_frames)
{
    const uint64_t watermark = Watermark(total_frames);
    const uint64_t spare = free_frames > watermark ? free_frames - watermark : 0;
    return std::min(free_frames / 4, spare);
}

uint64_t TrimMargin(uint64_t total_frames)
{
    return Watermark(total_frames) / 2;
}

size_t TrimToMargin(FeatherDisk::PageCache& cache, const FeatherDisk::PageSource& source,
                    uint64_t margin)
{
    if (source.Headroom() > 0)
    {
        return 0;
    }
    size_t total = 0;
    while (source.Headroom() < margin)
    {
        if (cache.Shrink(1) == 0)
        {
            break;
        }
        ++total;
    }
    return total;
}

} // namespace FeatherOS::BlockDevice
