#include "virtqueue.h"

#include "kernel/memory/page_allocator.h"

namespace FeatherOS::Virtio
{
namespace
{

constexpr uint16_t DescriptorDeviceWrite = 2;
constexpr uint16_t AvailableNoInterrupt = 1;
constexpr uint16_t MaximumSize = 256;

struct __attribute__((packed)) Descriptor
{
    uint64_t address;
    uint32_t length;
    uint16_t flags;
    uint16_t next;
};

struct __attribute__((packed)) UsedElement
{
    uint32_t id;
    uint32_t length;
};

constexpr uint32_t AvailableOffset(uint16_t size)
{
    return sizeof(Descriptor) * size;
}

// The device is QEMU running on the host's CPUs, so ordinary CPU fences order the ring updates
// it sees.
void Fence()
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

} // namespace

bool Virtqueue::Allocate(uint16_t queue_size, uint64_t hhdm_offset)
{
    if (memory != nullptr || queue_size == 0 || queue_size > MaximumSize ||
        (queue_size & (queue_size - 1)) != 0)
    {
        return false;
    }
    const uint32_t available_end = AvailableOffset(queue_size) + 6 + 2U * queue_size;
    const uint32_t used = (available_end + Memory::PageSize - 1) & ~(Memory::PageSize - 1);
    const uint32_t total = used + 6 + static_cast<uint32_t>(sizeof(UsedElement)) * queue_size;
    const uint64_t page_count = (total + Memory::PageSize - 1) / Memory::PageSize;
    uint64_t physical;
    if (!Memory::AllocateContiguousPageFrames(page_count, &physical))
    {
        return false;
    }
    if (physical > UINT64_MAX - hhdm_offset || hhdm_offset + physical > UINTPTR_MAX)
    {
        for (uint64_t page = 0; page < page_count; ++page)
        {
            Memory::FreePageFrame(physical + page * Memory::PageSize);
        }
        return false;
    }

    memory = reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(hhdm_offset + physical));
    memory_size = static_cast<uint32_t>(page_count * Memory::PageSize);
    physical_address = physical;
    used_offset = used;
    size = queue_size;
    Reset();
    return true;
}

void Virtqueue::Reset()
{
    for (uint32_t offset = 0; offset < memory_size; ++offset)
    {
        memory[offset] = 0;
    }
    next_available = 0;
    next_used = 0;
}

uint16_t Virtqueue::Size() const
{
    return size;
}

uint64_t Virtqueue::DescriptorAddress() const
{
    return physical_address;
}

uint64_t Virtqueue::AvailableAddress() const
{
    return physical_address + AvailableOffset(size);
}

uint64_t Virtqueue::UsedAddress() const
{
    return physical_address + used_offset;
}

void Virtqueue::SuppressInterrupts()
{
    *reinterpret_cast<volatile uint16_t*>(memory + AvailableOffset(size)) = AvailableNoInterrupt;
}

void Virtqueue::Post(uint16_t descriptor, uint64_t address, uint32_t length, bool device_writes)
{
    auto* descriptors = reinterpret_cast<volatile Descriptor*>(memory);
    descriptors[descriptor].address = address;
    descriptors[descriptor].length = length;
    descriptors[descriptor].flags = device_writes ? DescriptorDeviceWrite : 0;
    descriptors[descriptor].next = 0;

    volatile uint8_t* available = memory + AvailableOffset(size);
    auto* ring = reinterpret_cast<volatile uint16_t*>(available + 4);
    ring[next_available % size] = descriptor;
    ++next_available;
    // The device must see the descriptor and ring entry before the index that publishes them.
    Fence();
    *reinterpret_cast<volatile uint16_t*>(available + 2) = next_available;
    Fence();
}

bool Virtqueue::HasUsed() const
{
    return *reinterpret_cast<volatile uint16_t*>(memory + used_offset + 2) != next_used;
}

bool Virtqueue::PopUsed(uint16_t* descriptor, uint32_t* length)
{
    if (!HasUsed())
    {
        return false;
    }
    // Read the element only after seeing the index that published it.
    Fence();
    const auto* elements = reinterpret_cast<volatile UsedElement*>(memory + used_offset + 4);
    const volatile UsedElement& element = elements[next_used % size];
    *descriptor = static_cast<uint16_t>(element.id);
    *length = element.length;
    ++next_used;
    return true;
}

} // namespace FeatherOS::Virtio
