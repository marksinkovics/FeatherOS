#include "page_allocator.h"

#include "memory_map.h"

#include "kernel/sync/spinlock.h"

#include <stddef.h>

namespace
{

constinit FeatherOS::Sync::SpinLock allocator_lock;
volatile uint8_t* allocation_bitmap = nullptr;
uint64_t frame_count = 0;
uint64_t free_frame_count = 0;
uint64_t next_search_index = 0;
uint64_t bitmap_physical_address = 0;
uint64_t bitmap_frame_count = 0;
size_t bitmap_size_bytes = 0;
bool initialized = false;

bool GetUsableFrameBounds(const FeatherOS::Memory::Region& region, uint64_t* first, uint64_t* end)
{
    if (first == nullptr || end == nullptr || region.length == 0 ||
        region.length > UINT64_MAX - region.base)
    {
        return false;
    }

    if (region.type != FeatherOS::Memory::RegionType::Usable)
    {
        *first = 0;
        *end = 0;
        return true;
    }

    const uint64_t physical_end = region.base + region.length;
    *first = region.base / FeatherOS::Memory::PageSize;
    if (region.base % FeatherOS::Memory::PageSize != 0)
    {
        ++*first;
    }
    *end = physical_end / FeatherOS::Memory::PageSize;
    return true;
}

bool GetFrameCount(const FeatherOS::Memory::Region& region, uint64_t* count)
{
    uint64_t first;
    uint64_t end;
    if (!GetUsableFrameBounds(region, &first, &end))
    {
        return false;
    }
    *count = end > first ? end - first : 0;
    return true;
}

bool IsAllocated(uint64_t index)
{
    const uint8_t mask = static_cast<uint8_t>(1U << (index & 7));
    return (allocation_bitmap[index >> 3] & mask) != 0;
}

void SetAllocated(uint64_t index, bool allocated)
{
    const size_t byte_index = static_cast<size_t>(index >> 3);
    const uint8_t mask = static_cast<uint8_t>(1U << (index & 7));
    const uint8_t value = allocation_bitmap[byte_index];
    allocation_bitmap[byte_index] =
        allocated ? static_cast<uint8_t>(value | mask) : static_cast<uint8_t>(value & ~mask);
}

bool GetPhysicalAddress(uint64_t frame_index, uint64_t* physical_address)
{
    uint64_t base_index = 0;
    for (size_t region_index = 0; region_index < FeatherOS::Memory::RegionCount(); ++region_index)
    {
        FeatherOS::Memory::Region region;
        uint64_t region_frame_count;
        uint64_t first_frame;
        uint64_t end_frame;
        if (!FeatherOS::Memory::GetRegion(region_index, &region) ||
            !GetFrameCount(region, &region_frame_count) ||
            !GetUsableFrameBounds(region, &first_frame, &end_frame))
        {
            return false;
        }
        if (frame_index >= base_index && frame_index - base_index < region_frame_count)
        {
            const uint64_t frame = first_frame + frame_index - base_index;
            *physical_address = frame * FeatherOS::Memory::PageSize;
            return true;
        }
        base_index += region_frame_count;
    }
    return false;
}

bool GetFrameIndex(uint64_t physical_address, uint64_t* frame_index)
{
    if (physical_address % FeatherOS::Memory::PageSize != 0)
    {
        return false;
    }

    const uint64_t frame = physical_address / FeatherOS::Memory::PageSize;
    uint64_t base_index = 0;
    for (size_t region_index = 0; region_index < FeatherOS::Memory::RegionCount(); ++region_index)
    {
        FeatherOS::Memory::Region region;
        uint64_t region_frame_count;
        uint64_t first_frame;
        uint64_t end_frame;
        if (!FeatherOS::Memory::GetRegion(region_index, &region) ||
            !GetFrameCount(region, &region_frame_count) ||
            !GetUsableFrameBounds(region, &first_frame, &end_frame))
        {
            return false;
        }
        if (frame >= first_frame && frame < end_frame)
        {
            *frame_index = base_index + frame - first_frame;
            return true;
        }
        base_index += region_frame_count;
    }
    return false;
}

bool FindBitmapRegion(uint64_t required_frames, uint64_t* physical_address)
{
    for (size_t index = 0; index < FeatherOS::Memory::RegionCount(); ++index)
    {
        FeatherOS::Memory::Region region;
        uint64_t first_frame;
        uint64_t end_frame;
        if (!FeatherOS::Memory::GetRegion(index, &region) ||
            !GetUsableFrameBounds(region, &first_frame, &end_frame))
        {
            return false;
        }
        const uint64_t available_frames = end_frame > first_frame ? end_frame - first_frame : 0;
        if (region.type == FeatherOS::Memory::RegionType::Usable &&
            available_frames >= required_frames)
        {
            *physical_address = first_frame * FeatherOS::Memory::PageSize;
            return true;
        }
    }
    return false;
}

} // namespace

namespace FeatherOS::Memory
{

bool InitializePageAllocator(uint64_t hhdm_offset)
{
    if (initialized)
    {
        return false;
    }

    uint64_t total_frames = 0;
    for (size_t index = 0; index < RegionCount(); ++index)
    {
        Region region;
        uint64_t region_frame_count;
        if (!GetRegion(index, &region) || !GetFrameCount(region, &region_frame_count) ||
            region_frame_count > UINT64_MAX - total_frames)
        {
            return false;
        }
        total_frames += region_frame_count;
    }
    if (total_frames == 0 || total_frames > UINT64_MAX - 7)
    {
        return false;
    }

    const uint64_t bitmap_bytes = (total_frames + 7) / 8;
    if (bitmap_bytes > SIZE_MAX || bitmap_bytes > UINT64_MAX - (PageSize - 1))
    {
        return false;
    }
    const uint64_t bitmap_frames = (bitmap_bytes + PageSize - 1) / PageSize;
    uint64_t bitmap_physical;
    if (!FindBitmapRegion(bitmap_frames, &bitmap_physical) ||
        bitmap_physical > UINT64_MAX - hhdm_offset)
    {
        return false;
    }
    const uint64_t bitmap_virtual = hhdm_offset + bitmap_physical;
    if (bitmap_virtual > UINTPTR_MAX)
    {
        return false;
    }

    auto* bitmap = reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(bitmap_virtual));
    for (size_t index = 0; index < static_cast<size_t>(bitmap_bytes); ++index)
    {
        bitmap[index] = 0xFF;
    }

    allocation_bitmap = bitmap;
    frame_count = total_frames;
    free_frame_count = 0;
    bitmap_physical_address = bitmap_physical;
    bitmap_frame_count = bitmap_frames;
    bitmap_size_bytes = static_cast<size_t>(bitmap_bytes);

    uint64_t frame_index = 0;
    for (size_t region_index = 0; region_index < RegionCount(); ++region_index)
    {
        Region region;
        uint64_t region_frame_count;
        if (!GetRegion(region_index, &region) || !GetFrameCount(region, &region_frame_count))
        {
            allocation_bitmap = nullptr;
            frame_count = 0;
            return false;
        }
        if (region.type == RegionType::Usable)
        {
            for (uint64_t offset = 0; offset < region_frame_count; ++offset)
            {
                SetAllocated(frame_index + offset, false);
            }
            free_frame_count += region_frame_count;
        }
        frame_index += region_frame_count;
    }

    for (uint64_t offset = 0; offset < bitmap_frames; ++offset)
    {
        uint64_t reserved_index;
        if (!GetFrameIndex(bitmap_physical + offset * PageSize, &reserved_index) ||
            IsAllocated(reserved_index))
        {
            allocation_bitmap = nullptr;
            frame_count = 0;
            free_frame_count = 0;
            return false;
        }
        SetAllocated(reserved_index, true);
        --free_frame_count;
    }

    next_search_index = 0;
    initialized = true;
    return true;
}

bool AllocatePageFrame(uint64_t* physical_address)
{
    Sync::SpinLockGuard guard(allocator_lock);
    if (!initialized || physical_address == nullptr || free_frame_count == 0)
    {
        return false;
    }

    for (uint64_t index = next_search_index; index < frame_count; ++index)
    {
        if (!IsAllocated(index))
        {
            uint64_t address;
            if (!GetPhysicalAddress(index, &address))
            {
                return false;
            }
            SetAllocated(index, true);
            --free_frame_count;
            next_search_index = index + 1 == frame_count ? 0 : index + 1;
            *physical_address = address;
            return true;
        }
    }
    for (uint64_t index = 0; index < next_search_index; ++index)
    {
        if (!IsAllocated(index))
        {
            uint64_t address;
            if (!GetPhysicalAddress(index, &address))
            {
                return false;
            }
            SetAllocated(index, true);
            --free_frame_count;
            next_search_index = index + 1 == frame_count ? 0 : index + 1;
            *physical_address = address;
            return true;
        }
    }
    return false;
}

bool FreePageFrame(uint64_t physical_address)
{
    Sync::SpinLockGuard guard(allocator_lock);
    if (!initialized || physical_address % PageSize != 0 ||
        (physical_address >= bitmap_physical_address &&
         physical_address - bitmap_physical_address < bitmap_frame_count * PageSize))
    {
        return false;
    }

    uint64_t index;
    if (!GetFrameIndex(physical_address, &index) || !IsAllocated(index))
    {
        return false;
    }
    SetAllocated(index, false);
    ++free_frame_count;
    if (index < next_search_index)
    {
        next_search_index = index;
    }
    return true;
}

bool GetPageAllocatorStats(PageAllocatorStats* stats)
{
    Sync::SpinLockGuard guard(allocator_lock);
    if (!initialized || stats == nullptr)
    {
        return false;
    }
    stats->total_frames = frame_count;
    stats->free_frames = free_frame_count;
    stats->bitmap_physical_address = bitmap_physical_address;
    stats->bitmap_size_bytes = bitmap_size_bytes;
    return true;
}

} // namespace FeatherOS::Memory