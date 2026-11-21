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
// When no frame is free it asks the registered reclaimer for some and tries once more.
bool AllocatePageFrame(uint64_t* physical_address);
// For devices that need physically contiguous memory, such as a legacy virtqueue.
bool AllocateContiguousPageFrames(uint64_t count, uint64_t* physical_address);
bool FreePageFrame(uint64_t physical_address);
bool GetPageAllocatorStats(PageAllocatorStats* stats);
uint64_t FreeFrameCount();

// Called with no allocator lock, from any context that may allocate a frame (heap and page-table
// locks held), so it must never allocate, log or take such a lock; it returns the frames it freed.
using FrameReclaimer = uint64_t (*)(uint64_t wanted);
void SetFrameReclaimer(FrameReclaimer reclaim);
// For the host tests, which check that the reclaimer runs without it.
bool AllocatorLockHeld();

} // namespace FeatherOS::Memory

#endif