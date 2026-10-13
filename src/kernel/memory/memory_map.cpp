#include "memory_map.h"

#include <limine.h>

namespace
{

__attribute__((used,
               section(".limine_requests"))) volatile limine_memmap_request memory_map_request = {
    .id = LIMINE_MEMMAP_REQUEST, .revision = 0, .response = nullptr};

const limine_memmap_response* response = nullptr;
FeatherOS::Memory::Summary summary = {};

bool NormalizeRegion(const limine_memmap_entry* entry, FeatherOS::Memory::Region* region)
{
    if (entry == nullptr || region == nullptr || entry->length == 0 ||
        entry->length > UINT64_MAX - entry->base)
    {
        return false;
    }

    region->base = entry->base;
    region->length = entry->length;
    switch (entry->type)
    {
    case LIMINE_MEMMAP_USABLE:
        region->type = FeatherOS::Memory::RegionType::Usable;
        break;
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
        region->type = FeatherOS::Memory::RegionType::AcpiReclaimable;
        break;
    case LIMINE_MEMMAP_ACPI_NVS:
        region->type = FeatherOS::Memory::RegionType::AcpiNvs;
        break;
    case LIMINE_MEMMAP_BAD_MEMORY:
        region->type = FeatherOS::Memory::RegionType::BadMemory;
        break;
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
        region->type = FeatherOS::Memory::RegionType::BootloaderReclaimable;
        break;
    case LIMINE_MEMMAP_KERNEL_AND_MODULES:
        region->type = FeatherOS::Memory::RegionType::KernelAndModules;
        break;
    case LIMINE_MEMMAP_FRAMEBUFFER:
        region->type = FeatherOS::Memory::RegionType::Framebuffer;
        break;
    default:
        region->type = FeatherOS::Memory::RegionType::Reserved;
        break;
    }
    return true;
}

} // namespace

namespace FeatherOS::Memory
{

bool InitializeMemoryMap()
{
    response = memory_map_request.response;
    summary.region_count = 0;
    summary.total_bytes = 0;
    summary.usable_bytes = 0;
    summary.reserved_bytes = 0;

    if (response == nullptr || response->entries == nullptr || response->entry_count == 0 ||
        response->entry_count > SIZE_MAX)
    {
        response = nullptr;
        return false;
    }

    for (size_t index = 0; index < response->entry_count; ++index)
    {
        Region region;
        if (!NormalizeRegion(response->entries[index], &region))
        {
            response = nullptr;
            summary.region_count = 0;
            summary.total_bytes = 0;
            summary.usable_bytes = 0;
            summary.reserved_bytes = 0;
            return false;
        }

        uint64_t* total =
            region.type == RegionType::Usable ? &summary.usable_bytes : &summary.reserved_bytes;
        if (region.length > UINT64_MAX - summary.total_bytes || region.length > UINT64_MAX - *total)
        {
            response = nullptr;
            summary.region_count = 0;
            summary.total_bytes = 0;
            summary.usable_bytes = 0;
            summary.reserved_bytes = 0;
            return false;
        }
        summary.total_bytes += region.length;
        *total += region.length;
        ++summary.region_count;
    }

    return true;
}

size_t RegionCount()
{
    return summary.region_count;
}

bool GetRegion(size_t index, Region* region)
{
    return response != nullptr && index < summary.region_count &&
           NormalizeRegion(response->entries[index], region);
}

bool GetSummary(Summary* output)
{
    if (response == nullptr || output == nullptr)
    {
        return false;
    }
    output->region_count = summary.region_count;
    output->total_bytes = summary.total_bytes;
    output->usable_bytes = summary.usable_bytes;
    output->reserved_bytes = summary.reserved_bytes;
    return true;
}

} // namespace FeatherOS::Memory