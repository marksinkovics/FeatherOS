#ifndef FEATHEROS_NETWORK_IPV6_H
#define FEATHEROS_NETWORK_IPV6_H

#include "address.h"
#include "ethernet.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Network
{

constexpr size_t Ipv6HeaderSize = 40;
constexpr size_t MaximumIpv6PacketSize = MaximumPayloadSize;
constexpr size_t MaximumIpv6Payload = MaximumIpv6PacketSize - Ipv6HeaderSize;
constexpr uint8_t IpProtocolIcmpv6 = 58;
// Neighbor discovery messages must arrive with the hop limit they were sent with, which proves
// they never crossed a router.
constexpr uint8_t NeighborDiscoveryHopLimit = 255;

constexpr IpAddress Ipv6Unspecified = {.version = IpVersion::V6, .bytes = {}};
constexpr IpAddress Ipv6Loopback = {.version = IpVersion::V6,
                                    .bytes = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}};
constexpr IpAddress AllNodes = {.version = IpVersion::V6,
                                .bytes = {0xFF, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}};
constexpr IpAddress AllRouters = {.version = IpVersion::V6,
                                  .bytes = {0xFF, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2}};

struct Ipv6Packet
{
    IpAddress source;
    IpAddress destination;
    uint8_t next_header;
    uint8_t hop_limit;
    const uint8_t* payload;
    size_t payload_length;
};

// Checks the version and lengths and skips hop-by-hop, routing and destination options headers,
// so next_header names the upper layer. Fragments are refused, since the stack does not
// reassemble them. The payload points into data.
bool ParseIpv6Packet(const uint8_t* data, size_t length, Ipv6Packet* parsed);
// Builds a packet without extension headers around fields' payload. Returns the packet's length,
// or 0 when the payload does not fit.
size_t BuildIpv6Packet(uint8_t (&packet)[MaximumIpv6PacketSize], const Ipv6Packet& fields);

// fe80::/64 with the interface identifier derived from mac (modified EUI-64, RFC 4291): the
// universal/local bit flipped and ff:fe in the middle, so 52:54:00:12:34:56 gives
// fe80::5054:ff:fe12:3456.
IpAddress LinkLocalAddress(const MacAddress& mac);
// The first 64 bits of prefix with mac's interface identifier, for stateless autoconfiguration.
IpAddress PrefixAddress(const IpAddress& prefix, const MacAddress& mac);
// ff02::1:ffXX:XXXX with the address's last 24 bits; neighbor solicitations for the address go
// there.
IpAddress SolicitedNodeAddress(const IpAddress& address);
// 33:33 and the multicast address's last 32 bits.
MacAddress MulticastMac(const IpAddress& address);
bool IsLinkLocal(const IpAddress& address);
bool IsMulticast(const IpAddress& address);
bool SamePrefix64(const IpAddress& left, const IpAddress& right);

} // namespace FeatherOS::Network

#endif
