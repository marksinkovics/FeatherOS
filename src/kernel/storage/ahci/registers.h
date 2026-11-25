#ifndef FEATHEROS_STORAGE_AHCI_REGISTERS_H
#define FEATHEROS_STORAGE_AHCI_REGISTERS_H

#include <stdint.h>

namespace FeatherOS::Storage::Ahci
{

namespace Reg
{
constexpr uint32_t Cap = 0x00;
constexpr uint32_t Ghc = 0x04;
constexpr uint32_t Is = 0x08;
constexpr uint32_t Pi = 0x0C;
constexpr uint32_t Vs = 0x10;
constexpr uint32_t Cap2 = 0x24;
constexpr uint32_t Bohc = 0x28;
constexpr uint32_t PortBase = 0x100;
constexpr uint32_t PortStride = 0x80;
constexpr uint32_t MaxPorts = 32;
} // namespace Reg

namespace PortReg
{
constexpr uint32_t Clb = 0x00;
constexpr uint32_t Clbu = 0x04;
constexpr uint32_t Fb = 0x08;
constexpr uint32_t Fbu = 0x0C;
constexpr uint32_t Is = 0x10;
constexpr uint32_t Ie = 0x14;
constexpr uint32_t Cmd = 0x18;
constexpr uint32_t Tfd = 0x20;
constexpr uint32_t Sig = 0x24;
constexpr uint32_t Ssts = 0x28;
constexpr uint32_t Sctl = 0x2C;
constexpr uint32_t Serr = 0x30;
constexpr uint32_t Sact = 0x34;
constexpr uint32_t Ci = 0x38;
} // namespace PortReg

constexpr uint32_t PortRegister(uint32_t port, uint32_t offset)
{
    return Reg::PortBase + port * Reg::PortStride + offset;
}

namespace CapBit
{
constexpr uint32_t PortsMask = 0x1F;
constexpr uint32_t SlotsShift = 8;
constexpr uint32_t SlotsMask = 0x1F;
constexpr uint32_t Sam = 1U << 18;
constexpr uint32_t Sss = 1U << 27;
constexpr uint32_t Sncq = 1U << 30;
constexpr uint32_t S64a = 1U << 31;
} // namespace CapBit

namespace Cap2Bit
{
constexpr uint32_t Boh = 1U << 0;
} // namespace Cap2Bit

namespace GhcBit
{
constexpr uint32_t Reset = 1U << 0;
constexpr uint32_t InterruptEnable = 1U << 1;
constexpr uint32_t AhciEnable = 1U << 31;
} // namespace GhcBit

namespace BohcBit
{
constexpr uint32_t BiosOwned = 1U << 0;
constexpr uint32_t OsOwned = 1U << 1;
constexpr uint32_t BiosBusy = 1U << 4;
} // namespace BohcBit

namespace CmdBit
{
constexpr uint32_t Start = 1U << 0;
constexpr uint32_t SpinUp = 1U << 1;
constexpr uint32_t FisReceiveEnable = 1U << 4;
constexpr uint32_t FisReceiveRunning = 1U << 14;
constexpr uint32_t CommandListRunning = 1U << 15;
} // namespace CmdBit

namespace TfdBit
{
constexpr uint32_t Error = 1U << 0;
constexpr uint32_t DataRequest = 1U << 3;
constexpr uint32_t Busy = 1U << 7;
} // namespace TfdBit

namespace IsBit
{
constexpr uint32_t DeviceToHost = 1U << 0;
constexpr uint32_t PioSetup = 1U << 1;
constexpr uint32_t PhyRdyChange = 1U << 22;
constexpr uint32_t Overflow = 1U << 24;
constexpr uint32_t InterfaceFatal = 1U << 27;
constexpr uint32_t HostBusData = 1U << 28;
constexpr uint32_t HostBusFatal = 1U << 29;
constexpr uint32_t TaskFileError = 1U << 30;
} // namespace IsBit

namespace SstsField
{
constexpr uint32_t DetMask = 0xF;
constexpr uint32_t DetPresent = 3;
constexpr uint32_t DetNoPhy = 1;
} // namespace SstsField

namespace SctlField
{
constexpr uint32_t DetMask = 0xF;
constexpr uint32_t DetReset = 1;
} // namespace SctlField

struct Capabilities
{
    uint32_t port_count;
    uint32_t command_slots;
    bool ahci_only;
    bool staggered_spin_up;
    bool ncq;
    bool addresses_64bit;
};

constexpr Capabilities DecodeCap(uint32_t cap)
{
    Capabilities c{};
    c.port_count = (cap & CapBit::PortsMask) + 1;
    c.command_slots = ((cap >> CapBit::SlotsShift) & CapBit::SlotsMask) + 1;
    c.ahci_only = (cap & CapBit::Sam) != 0;
    c.staggered_spin_up = (cap & CapBit::Sss) != 0;
    c.ncq = (cap & CapBit::Sncq) != 0;
    c.addresses_64bit = (cap & CapBit::S64a) != 0;
    return c;
}

constexpr bool HasHandoff(uint32_t cap2)
{
    return (cap2 & Cap2Bit::Boh) != 0;
}

struct Version
{
    uint16_t major;
    uint16_t minor;
};

constexpr Version DecodeVersion(uint32_t vs)
{
    return {static_cast<uint16_t>(vs >> 16), static_cast<uint16_t>(vs & 0xFFFF)};
}

constexpr bool PortImplemented(uint32_t implemented, uint32_t port)
{
    return port < Reg::MaxPorts && ((implemented >> port) & 1) != 0;
}

} // namespace FeatherOS::Storage::Ahci

#endif
