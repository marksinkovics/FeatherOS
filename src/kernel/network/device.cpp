#include "device.h"

#include "kernel/arch/virtio_net.h"
#include "kernel/memory/page_allocator.h"
#include "kernel/scheduler.h"
#include "kernel/sync/spinlock.h"
#include "kernel/virtio/virtqueue.h"

namespace FeatherOS::Network::Device
{
namespace
{

constexpr uint16_t PreferredQueueSize = 256;
constexpr uint16_t BufferCount = 64;
constexpr uint32_t BufferSize = 2048;
constexpr uint32_t BuffersPerPage = Memory::PageSize / BufferSize;

struct BufferPool
{
    uint64_t physical[BufferCount];
    uint8_t* memory[BufferCount];
};

Virtio::Virtqueue receive_queue;
Virtio::Virtqueue transmit_queue;
BufferPool receive_buffers;
BufferPool transmit_buffers;
uint16_t free_transmit[BufferCount];
uint16_t free_transmit_count = 0;
uint32_t header_size = 0;
MacAddress mac_address = {};
bool ready = false;
constinit Sync::SpinLock receive_lock;
constinit Sync::SpinLock transmit_lock;
constinit Scheduler::WaitQueue receive_waiters;

bool AllocateBuffers(BufferPool* pool, uint64_t hhdm_offset)
{
    for (uint16_t index = 0; index < BufferCount; index += BuffersPerPage)
    {
        uint64_t page;
        if (!Memory::AllocatePageFrame(&page) || page > UINT64_MAX - hhdm_offset)
        {
            return false;
        }
        for (uint32_t part = 0; part < BuffersPerPage; ++part)
        {
            pool->physical[index + part] = page + part * BufferSize;
            pool->memory[index + part] = reinterpret_cast<uint8_t*>(
                static_cast<uintptr_t>(hhdm_offset + page + part * BufferSize));
        }
    }
    return true;
}

void OnInterrupt()
{
    if (!Arch::VirtioNet::AcknowledgeInterrupt())
    {
        return;
    }
    Sync::SpinLockGuard guard(receive_lock);
    Scheduler::WakeAll(receive_waiters);
}

// With both locks held and the device negotiated.
bool StartLocked()
{
    // Sent buffers are reclaimed on the next send instead.
    transmit_queue.SuppressInterrupts();
    if (!Arch::VirtioNet::SetQueue(Arch::VirtioNet::ReceiveQueue, receive_queue) ||
        !Arch::VirtioNet::SetQueue(Arch::VirtioNet::TransmitQueue, transmit_queue))
    {
        return false;
    }
    for (uint16_t index = 0; index < BufferCount; ++index)
    {
        receive_queue.Post(index, receive_buffers.physical[index], BufferSize, true);
        free_transmit[index] = index;
    }
    free_transmit_count = BufferCount;
    ready = Arch::VirtioNet::Start(OnInterrupt);
    Arch::VirtioNet::Notify(Arch::VirtioNet::ReceiveQueue);
    return ready;
}

} // namespace

bool Initialize(const void* platform_data, uint64_t hhdm_offset)
{
    if (ready || !Arch::VirtioNet::Find(platform_data) || !Arch::VirtioNet::Negotiate())
    {
        return false;
    }
    header_size = Arch::VirtioNet::HeaderSize();
    const uint16_t receive_size =
        Arch::VirtioNet::QueueSize(Arch::VirtioNet::ReceiveQueue, PreferredQueueSize);
    const uint16_t transmit_size =
        Arch::VirtioNet::QueueSize(Arch::VirtioNet::TransmitQueue, PreferredQueueSize);
    if (receive_size < BufferCount || transmit_size < BufferCount ||
        !receive_queue.Allocate(receive_size, hhdm_offset) ||
        !transmit_queue.Allocate(transmit_size, hhdm_offset) ||
        !AllocateBuffers(&receive_buffers, hhdm_offset) ||
        !AllocateBuffers(&transmit_buffers, hhdm_offset))
    {
        return false;
    }
    Arch::VirtioNet::ReadMacAddress(mac_address.bytes);
    Sync::SpinLockGuard receive_guard(receive_lock);
    Sync::SpinLockGuard transmit_guard(transmit_lock);
    return StartLocked();
}

void Stop()
{
    Sync::SpinLockGuard receive_guard(receive_lock);
    Sync::SpinLockGuard transmit_guard(transmit_lock);
    if (ready)
    {
        ready = false;
        Arch::VirtioNet::Reset();
    }
}

bool Restart()
{
    Sync::SpinLockGuard receive_guard(receive_lock);
    Sync::SpinLockGuard transmit_guard(transmit_lock);
    if (ready)
    {
        return true;
    }
    if (!Arch::VirtioNet::Negotiate())
    {
        return false;
    }
    receive_queue.Reset();
    transmit_queue.Reset();
    return StartLocked();
}

MacAddress Address()
{
    return mac_address;
}

bool Transmit(const uint8_t* frame, size_t length)
{
    if (!ready || frame == nullptr || length > MaximumFrameSize)
    {
        return false;
    }
    Sync::SpinLockGuard guard(transmit_lock);
    uint16_t descriptor;
    uint32_t used_length;
    while (transmit_queue.PopUsed(&descriptor, &used_length))
    {
        if (descriptor < BufferCount && free_transmit_count < BufferCount)
        {
            free_transmit[free_transmit_count++] = descriptor;
        }
    }
    if (free_transmit_count == 0)
    {
        return false;
    }
    descriptor = free_transmit[--free_transmit_count];
    uint8_t* buffer = transmit_buffers.memory[descriptor];
    // No checksum offload or segmentation, so the header is all zeros.
    for (uint32_t index = 0; index < header_size; ++index)
    {
        buffer[index] = 0;
    }
    for (size_t index = 0; index < length; ++index)
    {
        buffer[header_size + index] = frame[index];
    }
    transmit_queue.Post(descriptor, transmit_buffers.physical[descriptor],
                        static_cast<uint32_t>(header_size + length), false);
    Arch::VirtioNet::Notify(Arch::VirtioNet::TransmitQueue);
    return true;
}

size_t Receive(uint8_t (&frame)[MaximumFrameSize])
{
    // Under the lock throughout, so Stop and Restart never change the queue in between.
    Sync::SpinLockGuard guard(receive_lock);
    for (;;)
    {
        uint16_t descriptor;
        uint32_t length;
        while (!ready || !receive_queue.PopUsed(&descriptor, &length))
        {
            Scheduler::WaitOn(receive_waiters, receive_lock);
        }
        size_t frame_length = 0;
        if (descriptor < BufferCount && length > header_size &&
            length - header_size <= MaximumFrameSize)
        {
            frame_length = length - header_size;
            const uint8_t* buffer = receive_buffers.memory[descriptor] + header_size;
            for (size_t index = 0; index < frame_length; ++index)
            {
                frame[index] = buffer[index];
            }
        }
        if (descriptor < BufferCount)
        {
            receive_queue.Post(descriptor, receive_buffers.physical[descriptor], BufferSize, true);
            Arch::VirtioNet::Notify(Arch::VirtioNet::ReceiveQueue);
        }
        if (frame_length > 0)
        {
            return frame_length;
        }
    }
}

} // namespace FeatherOS::Network::Device
