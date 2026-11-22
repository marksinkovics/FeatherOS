#ifndef FEATHEROS_MEMORY_DMA_H
#define FEATHEROS_MEMORY_DMA_H

#include <stdint.h>

namespace FeatherOS::Memory::Dma
{

struct Buffer
{
    uint64_t physical;
    volatile uint8_t* address;
    uint64_t size;
};

// Zeroed, physically contiguous pages through the HHDM, all below the exclusive physical limit.
bool Allocate(uint64_t bytes, uint64_t below, Buffer* buffer);
void Free(const Buffer& buffer);

// Before a doorbell write or a cycle-bit handover.
void ProducerBarrier();
// After seeing a descriptor the device handed back.
void ConsumerBarrier();

} // namespace FeatherOS::Memory::Dma

#endif
