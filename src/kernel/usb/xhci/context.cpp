#include "context.h"

namespace FeatherOS::Usb::Xhci
{

namespace
{

constexpr uint32_t RouteMask = 0xFFFFF;
constexpr uint32_t MaximumInterval = 15;

volatile uint32_t* Words(volatile uint8_t* base, uint32_t offset)
{
    return reinterpret_cast<volatile uint32_t*>(base + offset);
}

void SetAddFlag(const ContextLayout& layout, volatile uint8_t* input, uint32_t context)
{
    volatile uint32_t* flags = Words(input, layout.ControlOffset() + ControlAddFlags);
    *flags = *flags | (1U << context);
}

uint32_t FloorLog2(uint32_t value)
{
    uint32_t result = 0;
    while (value > 1)
    {
        value >>= 1;
        ++result;
    }
    return result;
}

uint32_t Clamp(uint32_t value, uint32_t low, uint32_t high)
{
    return value < low ? low : (value > high ? high : value);
}

uint32_t EndpointTypeField(const EndpointDescriptor& endpoint)
{
    const bool in = (endpoint.address & 0x80U) != 0;
    switch (endpoint.type)
    {
    case EndpointType::Control:
        return 4;
    case EndpointType::Isochronous:
        return in ? 5 : 1;
    case EndpointType::Bulk:
        return in ? 6 : 2;
    case EndpointType::Interrupt:
        return in ? 7 : 3;
    }
    return 0;
}

} // namespace

ContextLayout::ContextLayout(bool csz64) : size_(csz64 ? 64 : 32) {}

uint32_t ContextLayout::ContextSize() const
{
    return size_;
}

uint32_t ContextLayout::ControlOffset() const
{
    return 0;
}

uint32_t ContextLayout::SlotOffset(bool input) const
{
    return input ? size_ : 0;
}

uint32_t ContextLayout::EndpointOffset(bool input, uint32_t dci) const
{
    return SlotOffset(input) + dci * size_;
}

uint32_t ContextLayout::InputBytes() const
{
    return (2 + EndpointContexts) * size_;
}

uint32_t ContextLayout::DeviceBytes() const
{
    return (1 + EndpointContexts) * size_;
}

uint8_t SpeedId(Speed speed)
{
    switch (speed)
    {
    case Speed::Full:
        return 1;
    case Speed::Low:
        return 2;
    case Speed::High:
        return 3;
    case Speed::Super:
        return 4;
    case Speed::SuperPlus:
        return 5;
    }
    return 0;
}

bool SpeedFromId(uint8_t id, Speed* speed)
{
    switch (id)
    {
    case 1:
        *speed = Speed::Full;
        return true;
    case 2:
        *speed = Speed::Low;
        return true;
    case 3:
        *speed = Speed::High;
        return true;
    case 4:
        *speed = Speed::Super;
        return true;
    case 5:
        *speed = Speed::SuperPlus;
        return true;
    default:
        return false;
    }
}

uint32_t EndpointContextIndex(const EndpointDescriptor& endpoint)
{
    const uint32_t number = endpoint.address & 0x0FU;
    if (number == 0)
    {
        return 1;
    }
    return number * 2 + ((endpoint.address & 0x80U) != 0 ? 1 : 0);
}

void WriteSlot(const ContextLayout& layout, volatile uint8_t* input, const Location& location,
               uint32_t entries, const HubInfo* hub)
{
    volatile uint32_t* slot = Words(input, layout.SlotOffset(true));
    for (uint32_t word = 0; word < layout.ContextSize() / 4; ++word)
    {
        slot[word] = 0;
    }
    const bool multi_tt = (hub != nullptr && hub->multi_tt) || location.tt_multi;
    slot[0] = (location.route & RouteMask) |
              (static_cast<uint32_t>(SpeedId(location.speed)) << 20) | (multi_tt ? 1U << 25 : 0) |
              (hub != nullptr ? 1U << 26 : 0) | ((entries & 0x1FU) << 27);
    slot[1] = (static_cast<uint32_t>(location.root_port) << 16) |
              (hub != nullptr ? static_cast<uint32_t>(hub->ports) << 24 : 0);
    slot[2] = (location.tt_hub_slot & 0xFFU) | (static_cast<uint32_t>(location.tt_port) << 8) |
              (hub != nullptr ? static_cast<uint32_t>(hub->think_time & 3U) << 16 : 0);
    SetAddFlag(layout, input, 0);
}

void WriteEndpoint(const ContextLayout& layout, volatile uint8_t* input,
                   const EndpointDescriptor& endpoint, Speed speed, uint64_t ring)
{
    const uint32_t dci = EndpointContextIndex(endpoint);
    volatile uint32_t* context = Words(input, layout.EndpointOffset(true, dci));
    for (uint32_t word = 0; word < layout.ContextSize() / 4; ++word)
    {
        context[word] = 0;
    }
    const bool periodic =
        endpoint.type == EndpointType::Interrupt || endpoint.type == EndpointType::Isochronous;
    const uint32_t max_packet = endpoint.max_packet_size & 0x7FFU;
    uint32_t burst = endpoint.max_burst;
    if (speed == Speed::High && endpoint.type != EndpointType::Control &&
        endpoint.type != EndpointType::Bulk)
    {
        burst = (endpoint.max_packet_size >> 11) & 3U;
    }
    const uint32_t mult = speed >= Speed::Super && endpoint.type == EndpointType::Isochronous
                              ? endpoint.mult & 3U
                              : 0;
    uint32_t esit = 0;
    if (periodic)
    {
        esit = speed >= Speed::Super && endpoint.bytes_per_interval != 0
                   ? endpoint.bytes_per_interval
                   : max_packet * (burst + 1);
    }
    uint32_t average = 1024;
    if (endpoint.type == EndpointType::Control)
    {
        average = 8;
    }
    else if (endpoint.type == EndpointType::Bulk)
    {
        average = 3072;
    }
    else if (periodic)
    {
        average = Clamp(esit, 0, 0xFFFF);
    }
    const uint32_t error_count = endpoint.type == EndpointType::Isochronous ? 0 : 3;
    context[0] =
        (mult << 8) |
        (static_cast<uint32_t>(IntervalExponent(speed, endpoint.type, endpoint.interval)) << 16) |
        ((esit >> 16) << 24);
    context[1] =
        (error_count << 1) | (EndpointTypeField(endpoint) << 3) | (burst << 8) | (max_packet << 16);
    context[2] = static_cast<uint32_t>(ring);
    context[3] = static_cast<uint32_t>(ring >> 32);
    context[4] = average | ((esit & 0xFFFFU) << 16);
    SetAddFlag(layout, input, dci);
}

void SetInputFlags(const ContextLayout& layout, volatile uint8_t* input, uint32_t drop,
                   uint32_t add)
{
    volatile uint32_t* drops = Words(input, layout.ControlOffset() + ControlDropFlags);
    *drops = *drops | drop;
    volatile uint32_t* adds = Words(input, layout.ControlOffset() + ControlAddFlags);
    *adds = *adds | add;
}

uint8_t IntervalExponent(Speed speed, EndpointType type, uint8_t interval)
{
    if (type != EndpointType::Interrupt && type != EndpointType::Isochronous)
    {
        return 0;
    }
    const uint32_t raw = interval == 0 ? 1 : interval;
    if (speed >= Speed::High)
    {
        return static_cast<uint8_t>(Clamp(raw - 1, 0, MaximumInterval));
    }
    if (type == EndpointType::Isochronous)
    {
        return static_cast<uint8_t>(Clamp(raw - 1 + 3, 3, 10));
    }
    return static_cast<uint8_t>(Clamp(FloorLog2(raw * 8), 3, 10));
}

} // namespace FeatherOS::Usb::Xhci
