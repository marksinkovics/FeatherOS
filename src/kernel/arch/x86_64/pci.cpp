#include "pci.h"

#include "port_io.h"

namespace FeatherOS::Arch::X86_64::Pci
{
namespace
{

constexpr uint16_t ConfigAddress = 0x0CF8;
constexpr uint16_t ConfigData = 0x0CFC;
constexpr uint8_t CommandRegister = 0x04;
constexpr uint8_t Bar0 = 0x10;
constexpr uint8_t InterruptLineRegister = 0x3C;
constexpr uint32_t CommandIoSpace = 1U;
constexpr uint32_t CommandBusMaster = 1U << 2;

uint32_t ConfigAddressFor(uint32_t bus, uint32_t slot, uint32_t function, uint8_t offset)
{
    return 0x80000000U | (bus << 16) | (slot << 11) | (function << 8) | (offset & 0xFCU);
}

uint32_t ReadConfig(uint32_t bus, uint32_t slot, uint32_t function, uint8_t offset)
{
    PortIo::Write32(ConfigAddress, ConfigAddressFor(bus, slot, function, offset));
    return PortIo::Read32(ConfigData);
}

void WriteConfig(uint32_t bus, uint32_t slot, uint32_t function, uint8_t offset, uint32_t value)
{
    PortIo::Write32(ConfigAddress, ConfigAddressFor(bus, slot, function, offset));
    PortIo::Write32(ConfigData, value);
}

} // namespace

bool FindIoDevice(uint16_t vendor_id, uint16_t device_id, Device* device)
{
    for (uint32_t bus = 0; bus < 256; ++bus)
    {
        for (uint32_t slot = 0; slot < 32; ++slot)
        {
            for (uint32_t function = 0; function < 8; ++function)
            {
                const uint32_t identity = ReadConfig(bus, slot, function, 0);
                if (static_cast<uint16_t>(identity) == 0xFFFF)
                {
                    break;
                }
                if (static_cast<uint16_t>(identity) != vendor_id ||
                    static_cast<uint16_t>(identity >> 16) != device_id)
                {
                    continue;
                }

                const uint32_t bar0 = ReadConfig(bus, slot, function, Bar0);
                if ((bar0 & 1U) == 0 || (bar0 & 0xFFFCU) == 0)
                {
                    return false;
                }
                const uint32_t command = ReadConfig(bus, slot, function, CommandRegister);
                // The upper half is the status register, whose set bits clear when written back.
                WriteConfig(bus, slot, function, CommandRegister,
                            (command & 0xFFFFU) | CommandIoSpace | CommandBusMaster);
                device->io_base = static_cast<uint16_t>(bar0 & 0xFFFCU);
                device->interrupt_line =
                    static_cast<uint8_t>(ReadConfig(bus, slot, function, InterruptLineRegister));
                return true;
            }
        }
    }
    return false;
}

} // namespace FeatherOS::Arch::X86_64::Pci
