#include "pci_ecam.h"

#include "kernel/arch/pci.h"
#include "kernel/arch/serial.h"
#include "kernel/memory/device_memory.h"
#include "kernel/sync/spinlock.h"

namespace FeatherOS::Arch::AArch64::PciEcam
{
namespace
{

constexpr uint64_t BusWindowSize = 1ULL << 20;

class EcamConfigSpace final : public FeatherOS::Pci::ConfigSpace
{
public:
    uint32_t Read32(FeatherOS::Pci::Address address, uint16_t offset) override
    {
        volatile uint32_t* word = Locate(address, offset);
        return word == nullptr ? 0xFFFFFFFFU : *word;
    }

    void Write32(FeatherOS::Pci::Address address, uint16_t offset, uint32_t value) override
    {
        volatile uint32_t* word = Locate(address, offset);
        if (word != nullptr)
        {
            *word = value;
        }
    }

    void Attach(const PciDeviceTree::HostBridge* host)
    {
        Sync::SpinLockGuard guard(lock);
        bridge = host;
        for (uintptr_t& base : bus_base)
        {
            base = 0;
        }
    }

private:
    volatile uint32_t* Locate(FeatherOS::Pci::Address address, uint16_t offset)
    {
        Sync::SpinLockGuard guard(lock);
        if (bridge == nullptr || address.bus < bridge->first_bus ||
            address.bus > bridge->last_bus || address.device >= 32 || address.function >= 8 ||
            offset >= 4096)
        {
            return nullptr;
        }
        if (bus_base[address.bus] == 0)
        {
            const uint64_t physical =
                bridge->ecam_base + (static_cast<uint64_t>(address.bus - bridge->first_bus) << 20);
            bus_base[address.bus] = Memory::MapDevice(physical, BusWindowSize);
            if (bus_base[address.bus] == 0)
            {
                return nullptr;
            }
        }
        const uintptr_t config = bus_base[address.bus] +
                                 (static_cast<uintptr_t>(address.device) << 15) +
                                 (static_cast<uintptr_t>(address.function) << 12) + (offset & ~3U);
        return reinterpret_cast<volatile uint32_t*>(config);
    }

    Sync::SpinLock lock;
    const PciDeviceTree::HostBridge* bridge GUARDED_BY(lock) = nullptr;
    uintptr_t bus_base[256] GUARDED_BY(lock) = {};
};

PciDeviceTree::HostBridge host_bridge;
EcamConfigSpace config_space;
bool initialized = false;
bool available = false;

bool AssignMissingBars(const FeatherOS::Pci::Function& function, void*)
{
    if ((function.header_type & 0x7F) != 0)
    {
        return true;
    }
    for (uint32_t index = 0; index < 6;)
    {
        FeatherOS::Pci::Bar bar;
        if (!FeatherOS::Pci::ReadBar(config_space, function, index, &bar))
        {
            ++index;
            continue;
        }
        uint64_t address;
        if (bar.size != 0 && bar.address == 0 && !bar.is_io && host_bridge.has_memory32 &&
            PciDeviceTree::AllocateFromWindow(host_bridge.memory32, bar.size, &address))
        {
            FeatherOS::Pci::AssignBar(config_space, function, index, bar, address);
        }
        index += bar.is_64bit ? 2 : 1;
    }
    return true;
}

} // namespace

const PciDeviceTree::HostBridge* Bridge()
{
    return available ? &host_bridge : nullptr;
}

} // namespace FeatherOS::Arch::AArch64::PciEcam

namespace FeatherOS::Arch::Pci
{

FeatherOS::Pci::ConfigSpace* Initialize(const void* platform_data)
{
    using namespace AArch64;
    if (PciEcam::initialized)
    {
        return PciEcam::available ? &PciEcam::config_space : nullptr;
    }
    PciEcam::initialized = true;
    if (!PciDeviceTree::FindHostBridge(platform_data, &PciEcam::host_bridge))
    {
        return nullptr;
    }
    if (!PciEcam::host_bridge.dma_coherent)
    {
        Serial::Write("KERN/W: PCI: host bridge is not dma-coherent; PCI disabled\n");
        return nullptr;
    }
    PciEcam::config_space.Attach(&PciEcam::host_bridge);
    PciEcam::available = true;
    FeatherOS::Pci::Enumerate(PciEcam::config_space, PciEcam::AssignMissingBars, nullptr);
    return &PciEcam::config_space;
}

bool RouteIntx(const FeatherOS::Pci::Function& function, uint32_t* irq)
{
    using namespace AArch64;
    if (!PciEcam::available)
    {
        return false;
    }
    FeatherOS::Pci::Address root;
    uint8_t pin;
    return FeatherOS::Pci::SwizzleToRoot(PciEcam::config_space, function, &root, &pin) &&
           PciDeviceTree::LookupInterrupt(PciEcam::host_bridge, root.bus, root.device,
                                          root.function, pin, irq);
}

} // namespace FeatherOS::Arch::Pci
