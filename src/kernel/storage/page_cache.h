#ifndef FEATHEROS_STORAGE_PAGE_CACHE_H
#define FEATHEROS_STORAGE_PAGE_CACHE_H

#include "fs/page_cache.h"

#include <stdint.h>

namespace FeatherOS::BlockDevice
{

// Sizes the cache from the free frames and registers its reclaimer; after the heap, before any
// volume is opened, and once.
bool InitializeCache(uint64_t hhdm_offset);
// Valid after InitializeCache.
FeatherDisk::PageCache& Cache();
// What the page allocator calls when it has no frame.
uint64_t ReclaimFrames(uint64_t wanted);
void BalanceCache();
uint64_t CachedBytes();

} // namespace FeatherOS::BlockDevice

#endif
