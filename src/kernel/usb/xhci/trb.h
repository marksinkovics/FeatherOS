#ifndef FEATHEROS_USB_XHCI_TRB_H
#define FEATHEROS_USB_XHCI_TRB_H

#include <stdint.h>

namespace FeatherOS::Usb::Xhci
{

// Dwords 0 and 1 are the parameter, dword 2 the status, dword 3 the control word whose bit 0 is
// the cycle bit.
struct Trb
{
    uint32_t parameter_low;
    uint32_t parameter_high;
    uint32_t status;
    uint32_t control;
};

constexpr uint32_t TrbSize = 16;

namespace TrbType
{
constexpr uint32_t Normal = 1;
constexpr uint32_t SetupStage = 2;
constexpr uint32_t DataStage = 3;
constexpr uint32_t StatusStage = 4;
constexpr uint32_t Isochronous = 5;
constexpr uint32_t Link = 6;
constexpr uint32_t EventData = 7;
constexpr uint32_t NoOp = 8;
constexpr uint32_t EnableSlot = 9;
constexpr uint32_t DisableSlot = 10;
constexpr uint32_t AddressDevice = 11;
constexpr uint32_t ConfigureEndpoint = 12;
constexpr uint32_t EvaluateContext = 13;
constexpr uint32_t ResetEndpoint = 14;
constexpr uint32_t StopEndpoint = 15;
constexpr uint32_t SetTrDequeue = 16;
constexpr uint32_t ResetDevice = 17;
constexpr uint32_t NoOpCommand = 23;
constexpr uint32_t TransferEvent = 32;
constexpr uint32_t CommandCompletionEvent = 33;
constexpr uint32_t PortStatusChangeEvent = 34;
constexpr uint32_t HostControllerEvent = 37;
} // namespace TrbType

namespace TrbControl
{
constexpr uint32_t Cycle = 1U << 0;
constexpr uint32_t ToggleCycle = 1U << 1;
constexpr uint32_t EvaluateNext = 1U << 1;
constexpr uint32_t InterruptOnShortPacket = 1U << 2;
constexpr uint32_t NoSnoop = 1U << 3;
constexpr uint32_t Chain = 1U << 4;
constexpr uint32_t InterruptOnCompletion = 1U << 5;
constexpr uint32_t ImmediateData = 1U << 6;
constexpr uint32_t BlockSetAddress = 1U << 9;
constexpr uint32_t DirectionIn = 1U << 16;
constexpr uint32_t TypeShift = 10;
constexpr uint32_t TypeMask = 0x3FU << TypeShift;
constexpr uint32_t EndpointShift = 16;
constexpr uint32_t EndpointMask = 0x1FU << EndpointShift;
constexpr uint32_t SlotShift = 24;
constexpr uint32_t SlotMask = 0xFFU << SlotShift;
} // namespace TrbControl

namespace CompletionCode
{
constexpr uint32_t Invalid = 0;
constexpr uint32_t Success = 1;
constexpr uint32_t DataBufferError = 2;
constexpr uint32_t BabbleDetected = 3;
constexpr uint32_t UsbTransactionError = 4;
constexpr uint32_t TrbError = 5;
constexpr uint32_t StallError = 6;
constexpr uint32_t NoSlotsAvailable = 9;
constexpr uint32_t ShortPacket = 13;
constexpr uint32_t CommandRingStopped = 24;
constexpr uint32_t CommandAborted = 25;
} // namespace CompletionCode

constexpr uint32_t CompletionShift = 24;
constexpr uint32_t ResidueMask = 0xFFFFFFU;

constexpr uint32_t TrbTypeOf(const Trb& trb)
{
    return (trb.control & TrbControl::TypeMask) >> TrbControl::TypeShift;
}

constexpr uint32_t TypeField(uint32_t type)
{
    return type << TrbControl::TypeShift;
}

constexpr uint64_t TrbParameter(const Trb& trb)
{
    return (static_cast<uint64_t>(trb.parameter_high) << 32) | trb.parameter_low;
}

constexpr Trb MakeTrb(uint32_t type, uint32_t flags, uint64_t parameter, uint32_t status)
{
    return {static_cast<uint32_t>(parameter), static_cast<uint32_t>(parameter >> 32), status,
            TypeField(type) | flags};
}

} // namespace FeatherOS::Usb::Xhci

#endif
