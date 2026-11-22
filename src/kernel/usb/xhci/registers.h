#ifndef FEATHEROS_USB_XHCI_REGISTERS_H
#define FEATHEROS_USB_XHCI_REGISTERS_H

#include <stdint.h>

namespace FeatherOS::Usb::Xhci
{

namespace Cap
{
constexpr uint32_t Length = 0x00;
constexpr uint32_t Version = 0x02;
constexpr uint32_t HcsParams1 = 0x04;
constexpr uint32_t HcsParams2 = 0x08;
constexpr uint32_t HcsParams3 = 0x0C;
constexpr uint32_t HccParams1 = 0x10;
constexpr uint32_t DoorbellOffset = 0x14;
constexpr uint32_t RuntimeOffset = 0x18;
constexpr uint32_t HccParams2 = 0x1C;
} // namespace Cap

namespace HcsParams1
{
constexpr uint32_t MaxSlotsMask = 0xFF;
constexpr uint32_t MaxPortsShift = 24;
} // namespace HcsParams1

namespace HccParams1
{
constexpr uint32_t Ac64 = 1U << 0;
constexpr uint32_t ContextSize64 = 1U << 2;
constexpr uint32_t PortPowerControl = 1U << 3;
constexpr uint32_t ExtendedCapabilityShift = 16;
} // namespace HccParams1

// HCSPARAMS2 splits the scratchpad count: the high five bits at 21, the low five at 27.
constexpr uint32_t ScratchpadCount(uint32_t hcs_params2)
{
    return (((hcs_params2 >> 21) & 0x1FU) << 5) | ((hcs_params2 >> 27) & 0x1FU);
}

// Extended capabilities: a dword header with the ID in bits 7:0 and the distance to the next
// capability, in dwords, in bits 15:8.
namespace ExtendedCapability
{
constexpr uint32_t LegacySupport = 1;
constexpr uint32_t SupportedProtocol = 2;
constexpr uint32_t MaximumSteps = 48;
constexpr uint32_t LegacyControlStatus = 4;
constexpr uint32_t BiosOwned = 1U << 16;
constexpr uint32_t OsOwned = 1U << 24;
// USB SMI enable, SMI on host system error, and the OS ownership, hardware and bar change enables.
constexpr uint32_t SmiEnableMask = (1U << 0) | (1U << 4) | (7U << 13);
constexpr uint32_t SmiStatusMask = 7U << 29;
constexpr uint32_t ProtocolName = 0x20425355;
} // namespace ExtendedCapability

// Offsets from the operational base (capability length).
namespace Op
{
constexpr uint32_t UsbCmd = 0x00;
constexpr uint32_t UsbSts = 0x04;
constexpr uint32_t PageSize = 0x08;
constexpr uint32_t DeviceNotification = 0x14;
constexpr uint32_t CommandRing = 0x18;
constexpr uint32_t Dcbaap = 0x30;
constexpr uint32_t Config = 0x38;
constexpr uint32_t PortBase = 0x400;
constexpr uint32_t PortStride = 0x10;
constexpr uint32_t PortSc = 0x0;
constexpr uint32_t PortPmsc = 0x4;
constexpr uint32_t PortLi = 0x8;
constexpr uint32_t PortHlpmc = 0xC;
} // namespace Op

constexpr uint32_t PortRegister(uint32_t port_number, uint32_t reg)
{
    return Op::PortBase + (port_number - 1) * Op::PortStride + reg;
}

// Offsets from the runtime base (RTSOFF).
namespace Runtime
{
constexpr uint32_t MicroframeIndex = 0x00;
constexpr uint32_t InterrupterBase = 0x20;
constexpr uint32_t InterrupterStride = 0x20;
constexpr uint32_t Iman = 0x00;
constexpr uint32_t Imod = 0x04;
constexpr uint32_t ErstSize = 0x08;
constexpr uint32_t ErstBase = 0x10;
constexpr uint32_t Erdp = 0x18;
} // namespace Runtime

constexpr uint32_t InterrupterRegister(uint32_t interrupter, uint32_t reg)
{
    return Runtime::InterrupterBase + interrupter * Runtime::InterrupterStride + reg;
}

namespace UsbCmd
{
constexpr uint32_t RunStop = 1U << 0;
constexpr uint32_t HostControllerReset = 1U << 1;
constexpr uint32_t InterrupterEnable = 1U << 2;
constexpr uint32_t HostSystemErrorEnable = 1U << 3;
} // namespace UsbCmd

namespace UsbSts
{
constexpr uint32_t HostControllerHalted = 1U << 0;
constexpr uint32_t HostSystemError = 1U << 2;
constexpr uint32_t EventInterrupt = 1U << 3;
constexpr uint32_t PortChangeDetect = 1U << 4;
constexpr uint32_t ControllerNotReady = 1U << 11;
constexpr uint32_t HostControllerError = 1U << 12;
} // namespace UsbSts

namespace CommandRingControl
{
constexpr uint64_t RingCycleState = 1U << 0;
constexpr uint64_t CommandStop = 1U << 1;
constexpr uint64_t CommandAbort = 1U << 2;
constexpr uint64_t CommandRingRunning = 1U << 3;
} // namespace CommandRingControl

namespace Iman
{
constexpr uint32_t InterruptPending = 1U << 0;
constexpr uint32_t InterruptEnable = 1U << 1;
} // namespace Iman

namespace PortSc
{
constexpr uint32_t CurrentConnect = 1U << 0;
constexpr uint32_t Enabled = 1U << 1;
constexpr uint32_t OverCurrentActive = 1U << 3;
constexpr uint32_t Reset = 1U << 4;
constexpr uint32_t LinkStateShift = 5;
constexpr uint32_t LinkStateMask = 0xFU << LinkStateShift;
constexpr uint32_t Power = 1U << 9;
constexpr uint32_t SpeedShift = 10;
constexpr uint32_t SpeedMask = 0xFU << SpeedShift;
constexpr uint32_t IndicatorMask = 3U << 14;
constexpr uint32_t LinkWriteStrobe = 1U << 16;
constexpr uint32_t ConnectChange = 1U << 17;
constexpr uint32_t EnableChange = 1U << 18;
constexpr uint32_t WarmResetChange = 1U << 19;
constexpr uint32_t OverCurrentChange = 1U << 20;
constexpr uint32_t ResetChange = 1U << 21;
constexpr uint32_t LinkStateChange = 1U << 22;
constexpr uint32_t ConfigErrorChange = 1U << 23;
constexpr uint32_t ColdAttachStatus = 1U << 24;
constexpr uint32_t WakeOnConnect = 1U << 25;
constexpr uint32_t WakeOnDisconnect = 1U << 26;
constexpr uint32_t WakeOnOverCurrent = 1U << 27;
constexpr uint32_t DeviceRemovable = 1U << 30;
constexpr uint32_t WarmReset = 1U << 31;

constexpr uint32_t ChangeMask = ConnectChange | EnableChange | WarmResetChange | OverCurrentChange |
                                ResetChange | LinkStateChange | ConfigErrorChange;
constexpr uint32_t PreservedMask =
    Power | IndicatorMask | LinkWriteStrobe | WakeOnConnect | WakeOnDisconnect | WakeOnOverCurrent;
} // namespace PortSc

// A write that keeps the port as it is and clears only the change bits named in clear_bits.
uint32_t PortscWriteValue(uint32_t current, uint32_t clear_bits);

} // namespace FeatherOS::Usb::Xhci

#endif
