#include "msi.h"

namespace FeatherOS::Pci
{
namespace
{

constexpr uint32_t MsiAddressLow = 4;
constexpr uint32_t MsiAddressHigh = 8;
constexpr uint32_t MsiControlEnable = 1U << 0;
constexpr uint32_t MsiControlMultipleEnable = 7U << 4;
constexpr uint32_t MsiControl64Bit = 1U << 7;
constexpr uint32_t MsiControlPerVectorMask = 1U << 8;
constexpr uint32_t MsixControlTableMask = 0x7FF;
constexpr uint32_t MsixControlFunctionMask = 1U << 14;
constexpr uint32_t MsixControlEnable = 1U << 15;
constexpr uint32_t MsixEntrySize = 16;
constexpr uint32_t MsixVectorControlMasked = 1U;

uint32_t ReadControl(ConfigSpace& config, const Function& function, uint16_t capability)
{
    return config.Read32(function.address, capability) >> 16;
}

void WriteControl(ConfigSpace& config, const Function& function, uint16_t capability,
                  uint32_t control)
{
    const uint32_t header = config.Read32(function.address, capability) & 0xFFFFU;
    config.Write32(function.address, capability, header | (control << 16));
}

volatile uint32_t& Word(uintptr_t base, uint32_t offset)
{
    return *reinterpret_cast<volatile uint32_t*>(base + offset);
}

} // namespace

bool EnableMsiX(ConfigSpace& config, const Function& function, uintptr_t table, uint16_t entry,
                const MsiMessage& message)
{
    const uint16_t capability = FindCapability(config, function, CapabilityMsix);
    if (capability == 0 || table == 0)
    {
        return false;
    }
    const uint32_t control = ReadControl(config, function, capability);
    if (entry > (control & MsixControlTableMask))
    {
        return false;
    }

    WriteControl(config, function, capability,
                 (control & ~MsixControlEnable) | MsixControlFunctionMask);
    const uintptr_t slot = table + static_cast<uintptr_t>(entry) * MsixEntrySize;
    Word(slot, 12) = MsixVectorControlMasked;
    Word(slot, 0) = static_cast<uint32_t>(message.address);
    Word(slot, 4) = static_cast<uint32_t>(message.address >> 32);
    Word(slot, 8) = message.data;
    Word(slot, 12) = 0;
    UpdateCommand(config, function, CommandInterruptDisable, 0);
    WriteControl(config, function, capability,
                 (control | MsixControlEnable) & ~MsixControlFunctionMask);
    return true;
}

bool EnableMsi(ConfigSpace& config, const Function& function, const MsiMessage& message)
{
    const uint16_t capability = FindCapability(config, function, CapabilityMsi);
    if (capability == 0 || message.data > 0xFFFF)
    {
        return false;
    }
    const uint32_t control = ReadControl(config, function, capability);
    const bool wide = (control & MsiControl64Bit) != 0;
    if (!wide && (message.address >> 32) != 0)
    {
        return false;
    }

    // One vector: a device that offers several is told to use only its first.
    const uint32_t cleared = control & ~(MsiControlEnable | MsiControlMultipleEnable);
    WriteControl(config, function, capability, cleared);
    config.Write32(function.address, capability + MsiAddressLow,
                   static_cast<uint32_t>(message.address) & ~3U);
    uint16_t data_offset = capability + MsiAddressHigh;
    if (wide)
    {
        config.Write32(function.address, capability + MsiAddressHigh,
                       static_cast<uint32_t>(message.address >> 32));
        data_offset = capability + 12;
    }
    config.Write32(function.address, data_offset, message.data);
    if ((control & MsiControlPerVectorMask) != 0)
    {
        config.Write32(function.address, data_offset + 4, 0);
    }
    UpdateCommand(config, function, CommandInterruptDisable, 0);
    WriteControl(config, function, capability, cleared | MsiControlEnable);
    return true;
}

} // namespace FeatherOS::Pci
