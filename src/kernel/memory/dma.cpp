#include "dma.h"

#include "kernel/memory/page_allocator.h"

namespace FeatherOS::Memory::Dma
{

bool Allocate(uint64_t bytes, uint64_t below, Buffer* buffer)
{
    if (buffer == nullptr || bytes == 0 || bytes > UINT64_MAX - (PageSize - 1))
    {
        return false;
    }
    const uint64_t count = (bytes + PageSize - 1) / PageSize;
    uint64_t physical;
    if (!AllocateContiguousPageFrames(count, &physical, below))
    {
        return false;
    }
    const uint64_t hhdm = HhdmOffset();
    if (physical > UINTPTR_MAX - hhdm)
    {
        for (uint64_t page = 0; page < count; ++page)
        {
            FreePageFrame(physical + page * PageSize);
        }
        return false;
    }
    auto* address = reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(hhdm + physical));
    for (uint64_t offset = 0; offset < count * PageSize; ++offset)
    {
        address[offset] = 0;
    }
    buffer->physical = physical;
    buffer->address = address;
    buffer->size = bytes;
    return true;
}

void Free(const Buffer& buffer)
{
    const uint64_t count = (buffer.size + PageSize - 1) / PageSize;
    for (uint64_t page = 0; page < count; ++page)
    {
        FreePageFrame(buffer.physical + page * PageSize);
    }
}

void ProducerBarrier()
{
#if defined(ARCH_aarch64)
    asm volatile("dmb oshst" ::: "memory");
#else
    asm volatile("" ::: "memory");
#endif
}

void ConsumerBarrier()
{
#if defined(ARCH_aarch64)
    asm volatile("dmb oshld" ::: "memory");
#else
    asm volatile("" ::: "memory");
#endif
}

} // namespace FeatherOS::Memory::Dma
