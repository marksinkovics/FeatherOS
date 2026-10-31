#ifndef FEATHEROS_X86_64_PCI_H
#define FEATHEROS_X86_64_PCI_H

#include <stdint.h>

namespace FeatherOS::Arch::X86_64::Pci
{

constexpr uint16_t VirtioVendorId = 0x1AF4;
// Transitional virtio devices, which keep the legacy I/O-port interface.
constexpr uint16_t VirtioNetworkLegacyDeviceId = 0x1000;
constexpr uint16_t VirtioBlockLegacyDeviceId = 0x1001;

struct Device
{
    uint16_t io_base;
    // The legacy PIC line the firmware routed the device's INTx pin to, or 0xFF for none.
    uint8_t interrupt_line;
};

// Finds the first function with these IDs whose BAR 0 is an I/O range, and enables its I/O space
// and bus mastering.
bool FindIoDevice(uint16_t vendor_id, uint16_t device_id, Device* device);

} // namespace FeatherOS::Arch::X86_64::Pci

#endif
