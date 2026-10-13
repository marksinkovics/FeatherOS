#ifndef FEATHEROS_MEMORY_MAP_H
#define FEATHEROS_MEMORY_MAP_H

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Memory
{

enum class RegionType : uint8_t
{
    Usable,
    Reserved,
    AcpiReclaimable,
    AcpiNvs,
    BadMemory,
    BootloaderReclaimable,
    KernelAndModules,
    Framebuffer,
};

struct Region
{
    uint64_t base;
    uint64_t length;
    RegionType type;
};

struct Summary
{
    size_t region_count;
    uint64_t total_bytes;
    uint64_t usable_bytes;
    uint64_t reserved_bytes;
};

bool InitializeMemoryMap();
size_t RegionCount();
bool GetRegion(size_t index, Region* region);
bool GetSummary(Summary* summary);

} // namespace FeatherOS::Memory

#endif