#ifndef FEATHEROS_USB_HOST_CONTROLLER_H
#define FEATHEROS_USB_HOST_CONTROLLER_H

#include <stdint.h>

namespace FeatherOS::Usb
{

enum class Speed : uint8_t
{
    Low = 1,
    Full,
    High,
    Super,
    SuperPlus,
};

// root_port is the controller's port number, counting from 1; a route holds one nibble per hub
// tier, the nearest hub's port in the lowest nibble.
struct Location
{
    Speed speed;
    uint8_t root_port;
    uint8_t depth;
    uint8_t tt_port;
    uint32_t route;
    uint32_t tt_hub_slot;
    bool tt_multi;
};

enum class EndpointType : uint8_t
{
    Control,
    Isochronous,
    Bulk,
    Interrupt,
};

// max_packet_size is wMaxPacketSize as read; the SuperSpeed fields come from the companion
// descriptor and stay 0 for other speeds.
struct EndpointDescriptor
{
    uint8_t address;
    EndpointType type;
    uint16_t max_packet_size;
    uint8_t interval;
    uint8_t max_burst;
    uint8_t mult;
    uint32_t bytes_per_interval;
};

struct HubInfo
{
    uint8_t ports;
    bool multi_tt;
    uint8_t think_time;
};

} // namespace FeatherOS::Usb

#endif
