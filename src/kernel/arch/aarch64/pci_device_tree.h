#ifndef FEATHEROS_AARCH64_PCI_DEVICE_TREE_H
#define FEATHEROS_AARCH64_PCI_DEVICE_TREE_H

#include <stdint.h>

namespace FeatherOS::Arch::AArch64::PciDeviceTree
{

constexpr uint32_t MaximumInterruptMapEntries = 64;

struct Window
{
    uint64_t pci_base;
    uint64_t cpu_base;
    uint64_t size;
    bool prefetchable;
    // The bump allocator's next free PCI address.
    uint64_t next;
};

struct InterruptMapEntry
{
    uint32_t child_address[3];
    uint32_t pin;
    // A GIC interrupt ID.
    uint32_t interrupt_id;
};

struct HostBridge
{
    uint64_t ecam_base;
    uint64_t ecam_size;
    uint8_t first_bus;
    uint8_t last_bus;
    bool dma_coherent;
    // The phandle of the MSI controller, or 0 for none.
    uint32_t msi_phandle;
    // The GICv2m frame msi_phandle names; has_msi_frame is false for any other MSI controller.
    bool has_msi_frame;
    uint64_t msi_frame_base;
    uint64_t msi_frame_size;
    bool has_io;
    bool has_memory32;
    bool has_memory64;
    Window io;
    Window memory32;
    Window memory64;
    uint32_t interrupt_mask[4];
    uint32_t interrupt_map_count;
    InterruptMapEntry interrupt_map[MaximumInterruptMapEntries];
};

// Reads the first enabled "pci-host-ecam-generic" node. A malformed node gives false.
bool FindHostBridge(const void* device_tree, HostBridge* bridge);

bool LookupInterrupt(const HostBridge& bridge, uint8_t bus, uint8_t device, uint8_t function,
                     uint8_t pin, uint32_t* interrupt_id);

// Hands out an address aligned to size (a power of two) from the window.
bool AllocateFromWindow(Window& window, uint64_t size, uint64_t* pci_address);

} // namespace FeatherOS::Arch::AArch64::PciDeviceTree

#endif
