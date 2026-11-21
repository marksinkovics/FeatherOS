#ifndef FEATHEROS_USB_XHCI_CONTEXT_H
#define FEATHEROS_USB_XHCI_CONTEXT_H

#include "kernel/usb/host_controller.h"

#include <stdint.h>

namespace FeatherOS::Usb::Xhci
{

// Every context is 32 or 64 bytes (HCCPARAMS1.CSZ); the input context is a control context, the
// slot and the 31 endpoint contexts, the device context the slot and the endpoints.
class ContextLayout
{
public:
    explicit ContextLayout(bool csz64);

    uint32_t ContextSize() const;
    uint32_t ControlOffset() const;
    uint32_t SlotOffset(bool input) const;
    uint32_t EndpointOffset(bool input, uint32_t dci) const;
    uint32_t InputBytes() const;
    uint32_t DeviceBytes() const;

private:
    uint32_t size_;
};

constexpr uint32_t EndpointContexts = 31;
constexpr uint32_t ControlDropFlags = 0;
constexpr uint32_t ControlAddFlags = 4;

uint8_t SpeedId(Speed speed);
bool SpeedFromId(uint8_t id, Speed* speed);

uint32_t EndpointContextIndex(const EndpointDescriptor& endpoint);

// Both write into an input context and set their add flag; the caller zeroed the page.
void WriteSlot(const ContextLayout& layout, volatile uint8_t* input, const Location& location,
               uint32_t entries, const HubInfo* hub);
// ring is the transfer ring's physical address with its cycle state in bit 0.
void WriteEndpoint(const ContextLayout& layout, volatile uint8_t* input,
                   const EndpointDescriptor& endpoint, Speed speed, uint64_t ring);

uint8_t IntervalExponent(Speed speed, EndpointType type, uint8_t interval);

} // namespace FeatherOS::Usb::Xhci

#endif
