#ifndef FEATHEROS_TESTS_SUPPORT_PHYSICAL_MEMORY_H
#define FEATHEROS_TESTS_SUPPORT_PHYSICAL_MEMORY_H

#include "kernel/memory/dma.h"

#include <cstdint>
#include <vector>

namespace FeatherOS::Testing
{

// Host memory that stands for physical memory: driver code writes through the buffer's address
// and a fake controller reads the same bytes through the physical address.
class PhysicalMemory
{
public:
    explicit PhysicalMemory(uint64_t base = 0x1'2000'0000ULL);
    PhysicalMemory(const PhysicalMemory&) = delete;
    PhysicalMemory& operator=(const PhysicalMemory&) = delete;
    ~PhysicalMemory();

    Memory::Dma::Buffer Allocate(uint64_t bytes);
    volatile uint8_t* Translate(uint64_t physical, uint64_t length) const;

private:
    struct Region
    {
        uint64_t physical;
        uint8_t* host;
        uint64_t size;
    };

    std::vector<Region> regions_;
    uint64_t next_physical_;
};

} // namespace FeatherOS::Testing

#endif
