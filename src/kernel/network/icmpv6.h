#ifndef FEATHEROS_NETWORK_ICMPV6_H
#define FEATHEROS_NETWORK_ICMPV6_H

#include "address.h"
#include "icmp.h"

#include <stddef.h>
#include <stdint.h>

// ICMPv6 (RFC 4443) and its neighbor discovery messages (RFC 4861). Builders leave the checksum
// 0; it covers a pseudo-header with both addresses, so it is computed once they are known.
namespace FeatherOS::Network
{

constexpr size_t Icmpv6ChecksumOffset = 2;
constexpr uint8_t Icmpv6DestinationUnreachable = 1;
constexpr uint8_t Icmpv6PortUnreachable = 4;
constexpr uint8_t Icmpv6EchoRequest = 128;
constexpr uint8_t Icmpv6EchoReply = 129;
constexpr uint8_t RouterSolicitation = 133;
constexpr uint8_t RouterAdvertisementType = 134;
constexpr uint8_t NeighborSolicitation = 135;
constexpr uint8_t NeighborAdvertisement = 136;
constexpr size_t MaximumNeighborMessageSize = 32;
// An error's packet stays within the minimum IPv6 MTU of 1280 bytes (RFC 4443 2.4 c), a 40-byte
// header included.
constexpr size_t MaximumIcmpv6ErrorSize = 1280 - 40;

struct NeighborMessage
{
    uint8_t type;
    bool router;
    bool solicited;
    bool override_entry;
    IpAddress target;
    // The source link-layer address option of a solicitation, or the target one of an
    // advertisement.
    bool has_link_address;
    MacAddress link_address;
};

struct RouterAdvertisement
{
    // Seconds the sender may be used as the default router; 0 means it is not one.
    uint16_t router_lifetime;
    bool has_link_address;
    MacAddress link_address;
    // The first prefix that allows stateless autoconfiguration.
    bool has_prefix;
    IpAddress prefix;
    uint8_t prefix_length;
    uint32_t valid_lifetime;
    // The first recursive DNS server option's first address (RFC 8106), and its lifetime in
    // seconds.
    bool has_name_server;
    IpAddress name_server;
    uint32_t name_server_lifetime;
};

// An echo request or reply with code 0; the IP layer has already checked the checksum.
bool ParseIcmpv6Echo(const uint8_t* message, size_t length, IcmpEcho* echo);
// A neighbor solicitation or advertisement with code 0 and well-formed options.
bool ParseNeighborMessage(const uint8_t* message, size_t length, NeighborMessage* parsed);
bool ParseRouterAdvertisement(const uint8_t* message, size_t length, RouterAdvertisement* parsed);
// Without source_link, as duplicate address detection sends it from the unspecified address.
// Each builder returns the message's length.
size_t BuildNeighborSolicitation(uint8_t (&message)[MaximumNeighborMessageSize],
                                 const IpAddress& target, const MacAddress* source_link);
// With the override flag, since the address is ours.
size_t BuildNeighborAdvertisement(uint8_t (&message)[MaximumNeighborMessageSize],
                                  const IpAddress& target, const MacAddress& link, bool solicited);
size_t BuildRouterSolicitation(uint8_t (&message)[MaximumNeighborMessageSize],
                               const MacAddress* source_link);
// Quotes as much of the IPv6 packet as fits.
size_t BuildIcmpv6PortUnreachable(uint8_t (&message)[MaximumIcmpv6ErrorSize], const uint8_t* packet,
                                  size_t length);
// A destination unreachable error quoting an IPv6 header and at least the 8 bytes after it, whose
// next header is taken as the protocol.
bool ParseIcmpv6Unreachable(const uint8_t* message, size_t length, IcmpUnreachable* parsed);

} // namespace FeatherOS::Network

#endif
