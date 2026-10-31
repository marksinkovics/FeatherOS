#ifndef FEATHEROS_AARCH64_VIRTIO_MMIO_H
#define FEATHEROS_AARCH64_VIRTIO_MMIO_H

#include <stdint.h>

namespace FeatherOS::Arch::AArch64::VirtioMmio
{

constexpr uint32_t BlockDeviceId = 2;
constexpr uint32_t InputDeviceId = 18;
constexpr uint32_t NetworkDeviceId = 1;

struct Device
{
    uintptr_t base;
    // A GIC interrupt ID, or 0 when the node lists no interrupt.
    uint32_t interrupt_id;
};

// Finds the first enabled virtio-mmio node in the device tree whose device has this ID (version
// 2 transports only) and maps its registers.
bool Find(const void* device_tree, uint32_t device_id, Device* device);

} // namespace FeatherOS::Arch::AArch64::VirtioMmio

#endif
