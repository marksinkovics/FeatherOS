#include "pci.h"

#include "kernel/arch/pci.h"
#include "kernel/sync/spinlock.h"
#include "port_io.h"

namespace FeatherOS::Arch::X86_64::Pci
{
namespace
{

constexpr uint16_t ConfigAddress = 0x0CF8;
constexpr uint16_t ConfigData = 0x0CFC;

// The address and data ports are a pair, so two CPUs must not interleave their accesses.
constinit Sync::SpinLock port_lock;

class PortConfigSpace final : public FeatherOS::Pci::ConfigSpace
{
public:
    uint32_t Read32(FeatherOS::Pci::Address address, uint16_t offset) override
    {
        Sync::SpinLockGuard guard(port_lock);
        PortIo::Write32(ConfigAddress, AddressFor(address, offset));
        return PortIo::Read32(ConfigData);
    }

    void Write32(FeatherOS::Pci::Address address, uint16_t offset, uint32_t value) override
    {
        Sync::SpinLockGuard guard(port_lock);
        PortIo::Write32(ConfigAddress, AddressFor(address, offset));
        PortIo::Write32(ConfigData, value);
    }

private:
    static uint32_t AddressFor(FeatherOS::Pci::Address address, uint16_t offset)
    {
        return 0x80000000U | (static_cast<uint32_t>(address.bus) << 16) |
               (static_cast<uint32_t>(address.device) << 11) |
               (static_cast<uint32_t>(address.function) << 8) | (offset & 0xFCU);
    }
};

PortConfigSpace config_space;

struct Search
{
    uint16_t vendor_id;
    uint16_t device_id;
    FeatherOS::Pci::Function function;
    bool found;
};

bool Match(const FeatherOS::Pci::Function& function, void* context)
{
    auto* search = static_cast<Search*>(context);
    if (function.vendor != search->vendor_id || function.device != search->device_id)
    {
        return true;
    }
    search->function = function;
    search->found = true;
    return false;
}

} // namespace

bool FindIoDevice(uint16_t vendor_id, uint16_t device_id, Device* device)
{
    Search search = {vendor_id, device_id, {}, false};
    FeatherOS::Pci::Enumerate(config_space, Match, &search);
    if (!search.found)
    {
        return false;
    }

    FeatherOS::Pci::Bar bar;
    if (!FeatherOS::Pci::ReadBar(config_space, search.function, 0, &bar) || !bar.is_io ||
        bar.address == 0)
    {
        return false;
    }
    FeatherOS::Pci::UpdateCommand(config_space, search.function,
                                  FeatherOS::Pci::CommandIoSpace | FeatherOS::Pci::CommandBusMaster,
                                  0);
    device->io_base = static_cast<uint16_t>(bar.address);
    device->interrupt_line = static_cast<uint8_t>(
        config_space.Read32(search.function.address, FeatherOS::Pci::InterruptRegister));
    return true;
}

} // namespace FeatherOS::Arch::X86_64::Pci

namespace FeatherOS::Arch::Pci
{

FeatherOS::Pci::ConfigSpace* Initialize(const void*)
{
    return &X86_64::Pci::config_space;
}

bool RouteIntx(const FeatherOS::Pci::Function&, uint32_t*)
{
    return false;
}

} // namespace FeatherOS::Arch::Pci
