#include "physical_memory.h"

#include <cstdlib>
#include <cstring>

namespace FeatherOS::Testing
{

namespace
{

constexpr uint64_t PageSize = 4096;

} // namespace

PhysicalMemory::~PhysicalMemory()
{
    for (const Region& region : regions_)
    {
        std::free(region.host);
    }
}

PhysicalMemory::PhysicalMemory(uint64_t base) : next_physical_(base)
{
}

Memory::Dma::Buffer PhysicalMemory::Allocate(uint64_t bytes)
{
    const uint64_t size = (bytes + PageSize - 1) / PageSize * PageSize;
    auto* host = static_cast<uint8_t*>(std::aligned_alloc(PageSize, size));
    std::memset(host, 0, size);
    regions_.push_back({next_physical_, host, size});
    Memory::Dma::Buffer buffer{next_physical_, host, bytes};
    next_physical_ += size + PageSize;
    return buffer;
}

volatile uint8_t* PhysicalMemory::Translate(uint64_t physical, uint64_t length) const
{
    for (const Region& region : regions_)
    {
        if (physical >= region.physical && length <= region.size &&
            physical - region.physical <= region.size - length)
        {
            return region.host + (physical - region.physical);
        }
    }
    return nullptr;
}

} // namespace FeatherOS::Testing
