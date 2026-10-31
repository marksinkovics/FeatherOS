#ifndef FEATHEROS_VIRTIO_VIRTQUEUE_H
#define FEATHEROS_VIRTIO_VIRTQUEUE_H

#include <stdint.h>

namespace FeatherOS::Virtio
{

// A split virtqueue in the legacy layout: descriptors, the available ring, then the used ring on
// the next page boundary, all physically contiguous. Virtio 1.0 devices accept that layout too.
// Each buffer is a single descriptor, posted under its own index.
class Virtqueue
{
public:
    bool Allocate(uint16_t size, uint64_t hhdm_offset);
    // Empties the rings for a device that was reset, keeping the memory.
    void Reset();
    uint16_t Size() const;
    uint64_t DescriptorAddress() const;
    uint64_t AvailableAddress() const;
    uint64_t UsedAddress() const;
    // Asks the device not to interrupt when it returns buffers from this queue.
    void SuppressInterrupts();
    // The caller notifies the device afterwards.
    void Post(uint16_t descriptor, uint64_t address, uint32_t length, bool device_writes);
    bool HasUsed() const;
    bool PopUsed(uint16_t* descriptor, uint32_t* length);

private:
    volatile uint8_t* memory = nullptr;
    uint64_t physical_address = 0;
    uint32_t used_offset = 0;
    uint32_t memory_size = 0;
    uint16_t size = 0;
    uint16_t next_available = 0;
    uint16_t next_used = 0;
};

} // namespace FeatherOS::Virtio

#endif
