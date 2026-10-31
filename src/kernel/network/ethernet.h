#ifndef FEATHEROS_NETWORK_ETHERNET_H
#define FEATHEROS_NETWORK_ETHERNET_H

#include "address.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Network
{

constexpr size_t EthernetHeaderSize = 14;
// Without the frame check sequence, which the device adds and removes.
constexpr size_t MinimumFrameSize = 60;
constexpr size_t MaximumFrameSize = 1514;
constexpr size_t MaximumPayloadSize = MaximumFrameSize - EthernetHeaderSize;
constexpr uint16_t EtherTypeIpv4 = 0x0800;
constexpr uint16_t EtherTypeArp = 0x0806;
constexpr uint16_t EtherTypeIpv6 = 0x86DD;

struct EthernetFrame
{
    MacAddress destination;
    MacAddress source;
    uint16_t type;
    const uint8_t* payload;
    size_t payload_length;
};

bool ParseEthernetFrame(const uint8_t* frame, size_t length, EthernetFrame* parsed);
// Builds a frame around the payload, padded to the minimum size. Returns the frame's length, or
// 0 when the payload does not fit.
size_t BuildEthernetFrame(uint8_t (&frame)[MaximumFrameSize], const MacAddress& destination,
                          const MacAddress& source, uint16_t type, const uint8_t* payload,
                          size_t payload_length);

} // namespace FeatherOS::Network

#endif
