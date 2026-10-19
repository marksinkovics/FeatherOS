#ifndef FEATHEROS_PAGE_ALLOCATOR_H
#define FEATHEROS_PAGE_ALLOCATOR_H

#include <stdint.h>

namespace FeatherOS::Memory
{

constexpr uint64_t PageSize = 4096;

struct PageAllocatorStats
{
    uint64_t total_frames;
    uint64_t free_frames;
    uint64_t bitmap_physical_address;
    uint64_t bitmap_size_bytes;
};

bool InitializePageAllocator(uint64_t hhdm_offset);
bool AllocatePageFrame(uint64_t* physical_address);
bool FreePageFrame(uint64_t physical_address);
bool GetPageAllocatorStats(PageAllocatorStats* stats);

} // namespace FeatherOS::Memory

#endif