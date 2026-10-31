#ifndef FEATHEROS_ARCH_VIRTIO_NET_H
#define FEATHEROS_ARCH_VIRTIO_NET_H

#include "kernel/virtio/virtqueue.h"

#include <stdint.h>

// The transport half of the virtio-net driver: legacy PCI on x86-64, virtio 1.0 MMIO on AArch64.
namespace FeatherOS::Arch::VirtioNet
{

constexpr uint16_t ReceiveQueue = 0;
constexpr uint16_t TransmitQueue = 1;

bool Find(const void* platform_data);
// Resets the device and negotiates features; the queues and Start follow.
bool Negotiate();
// Stops the device's DMA and interrupts; it forgets its queues until Negotiate and Start again.
void Reset();
// The virtio-net header before each frame: 10 bytes on a legacy device, 12 on a virtio 1.0 one.
uint32_t HeaderSize();
void ReadMacAddress(uint8_t* address);
// A legacy device fixes each queue's size; a virtio 1.0 one takes any size up to its maximum.
uint16_t QueueSize(uint16_t queue, uint16_t preferred);
bool SetQueue(uint16_t queue, const Virtio::Virtqueue& virtqueue);
bool Start(void (*interrupt_handler)());
void Notify(uint16_t queue);
// Returns whether the device raised the interrupt, and clears it.
bool AcknowledgeInterrupt();

} // namespace FeatherOS::Arch::VirtioNet

#endif
