#include "device_memory.h"

#include "kernel/memory/page_allocator.h"
#include "kernel/memory/virtual_memory.h"
#include "kernel/sync/spinlock.h"

namespace FeatherOS::Memory
{
namespace
{

// Device registers appear at a fixed offset from their physical address in the kernel half,
// so they stay reachable whichever address space is active.
constexpr uint64_t DeviceWindowBase = 0xFFFFF00000000000;
constexpr uint64_t DeviceWindowSize = 1ULL << 40;
constexpr uint32_t MaximumDeviceRanges = 64;

struct Range
{
    uint64_t begin;
    uint64_t end;
};

constinit Sync::SpinLock device_lock;
Range mapped_ranges[MaximumDeviceRanges] GUARDED_BY(device_lock);
uint32_t mapped_range_count GUARDED_BY(device_lock) = 0;

bool IsMapped(uint64_t physical_page) REQUIRES(device_lock)
{
    for (uint32_t index = 0; index < mapped_range_count; ++index)
    {
        if (physical_page >= mapped_ranges[index].begin && physical_page < mapped_ranges[index].end)
        {
            return true;
        }
    }
    return false;
}

bool Record(uint64_t begin, uint64_t end) REQUIRES(device_lock)
{
    for (uint32_t index = 0; index < mapped_range_count; ++index)
    {
        Range& range = mapped_ranges[index];
        if (begin <= range.end && end >= range.begin)
        {
            range.begin = begin < range.begin ? begin : range.begin;
            range.end = end > range.end ? end : range.end;
            return true;
        }
    }
    if (mapped_range_count == MaximumDeviceRanges)
    {
        return false;
    }
    mapped_ranges[mapped_range_count++] = {begin, end};
    return true;
}

} // namespace

uintptr_t MapDevice(uint64_t physical_address, uint64_t size)
{
    if (size == 0 || physical_address >= DeviceWindowSize ||
        size > DeviceWindowSize - physical_address)
    {
        return 0;
    }
    const uint64_t first_page = physical_address & ~(PageSize - 1);
    const uint64_t end_page = (physical_address + size + PageSize - 1) & ~(PageSize - 1);

    Sync::SpinLockGuard guard(device_lock);
    for (uint64_t page = first_page; page < end_page; page += PageSize)
    {
        if (!IsMapped(page) && !MapPage(DeviceWindowBase + page, page,
                                        PageFlags::Read | PageFlags::Write | PageFlags::Device))
        {
            // Pages mapped so far stay recorded, or a retry would map them twice.
            if (page > first_page)
            {
                Record(first_page, page);
            }
            return 0;
        }
    }
    if (!Record(first_page, end_page))
    {
        return 0;
    }
    return static_cast<uintptr_t>(DeviceWindowBase + physical_address);
}

} // namespace FeatherOS::Memory
