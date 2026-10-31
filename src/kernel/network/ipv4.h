#ifndef FEATHEROS_NETWORK_IPV4_H
#define FEATHEROS_NETWORK_IPV4_H

#include "address.h"
#include "ethernet.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Network
{

constexpr size_t Ipv4HeaderSize = 20;
constexpr size_t MaximumIpv4PacketSize = MaximumPayloadSize;
constexpr size_t MaximumIpv4Payload = MaximumIpv4PacketSize - Ipv4HeaderSize;
constexpr uint8_t IpProtocolIcmp = 1;
constexpr uint8_t DefaultTimeToLive = 64;
constexpr Ipv4Address Ipv4Broadcast = 0xFFFFFFFF;

struct Ipv4Packet
{
    Ipv4Address source;
    Ipv4Address destination;
    uint8_t protocol;
    const uint8_t* payload;
    size_t payload_length;
};

// Checks the version, lengths and header checksum, and skips any options. Fragments are refused,
// since the stack does not reassemble them. The payload points into data.
bool ParseIpv4Packet(const uint8_t* data, size_t length, Ipv4Packet* parsed);
// Builds a packet without options around fields' payload. Returns the packet's length, or 0 when
// the payload does not fit.
size_t BuildIpv4Packet(uint8_t (&packet)[MaximumIpv4PacketSize], const Ipv4Packet& fields,
                       uint16_t identification);
// 127.0.0.0/8.
bool IsIpv4Loopback(Ipv4Address address);

} // namespace FeatherOS::Network

#endif
