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
constexpr uint32_t MaximumDevicePages = 128;

constinit Sync::SpinLock device_lock;
uint64_t mapped_pages[MaximumDevicePages];
uint32_t mapped_page_count = 0;

bool IsMapped(uint64_t physical_page)
{
    for (uint32_t index = 0; index < mapped_page_count; ++index)
    {
        if (mapped_pages[index] == physical_page)
        {
            return true;
        }
    }
    return false;
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
        if (IsMapped(page))
        {
            continue;
        }
        if (mapped_page_count == MaximumDevicePages ||
            !MapPage(DeviceWindowBase + page, page,
                     PageFlags::Read | PageFlags::Write | PageFlags::Device))
        {
            return 0;
        }
        mapped_pages[mapped_page_count++] = page;
    }
    return static_cast<uintptr_t>(DeviceWindowBase + physical_address);
}

} // namespace FeatherOS::Memory
