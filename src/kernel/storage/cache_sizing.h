#ifndef FEATHEROS_STORAGE_CACHE_SIZING_H
#define FEATHEROS_STORAGE_CACHE_SIZING_H

#include "fs/page_cache.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::BlockDevice
{

constexpr uint64_t MinimumWatermark = 1024;

// Free frames the cache never takes: max(total / 32, MinimumWatermark).
uint64_t Watermark(uint64_t total_frames);
// A quarter of the free frames, and never the part below the watermark.
uint64_t CacheLimit(uint64_t free_frames, uint64_t total_frames);
// Once below the watermark, a trim goes on until the headroom is half the watermark.
uint64_t TrimMargin(uint64_t total_frames);
// Shrinks one page at a time while the source has no headroom, until it has margin; returns the
// pages freed.
size_t TrimToMargin(FeatherDisk::PageCache& cache, const FeatherDisk::PageSource& source,
                    uint64_t margin);

} // namespace FeatherOS::BlockDevice

#endif
