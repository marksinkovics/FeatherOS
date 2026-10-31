#ifndef FEATHEROS_NETWORK_NEIGHBOR_DISCOVERY_H
#define FEATHEROS_NETWORK_NEIGHBOR_DISCOVERY_H

#include "address.h"
#include "ipv6.h"

#include <stdint.h>

// IPv6 addresses for the interface, from stateless autoconfiguration (RFC 4862), and neighbor
// discovery (RFC 4861). A new address is optimistic (RFC 4429): usable at once while duplicate
// address detection runs for a second, and dropped if another host claims it meanwhile.
namespace FeatherOS::Network
{

// Unspecified addresses for what is missing.
struct Ipv6Configuration
{
    IpAddress link_local;
    IpAddress global;
    uint8_t prefix_length;
    IpAddress router;
};

// Takes the link-local address and asks for a router; the advertisement's prefix gives the
// global address.
void StartIpv6();
// Waits up to a second after StartIpv6 for the router's advertisement.
void WaitForRouter();
// Drops the addresses, the router and its name server, as when the link goes down.
void ForgetIpv6();
void ReadIpv6Configuration(Ipv6Configuration* configuration);
// The link-local or global address, unless detection found it to be a duplicate.
bool IsOwnIpv6Address(const IpAddress& address);
// All nodes, or the solicited-node group of one of our addresses.
bool IsJoinedIpv6Group(const IpAddress& address);
// The destination itself when it is one of ours or ::1, the link-local address for link-local
// and multicast destinations, otherwise the global one; unspecified when there is none.
IpAddress SelectIpv6Source(const IpAddress& destination);
// The destination itself when it is on the link, otherwise the default router. False without
// one.
bool Ipv6NextHop(const IpAddress& destination, IpAddress* next_hop);
// Multicasts a solicitation for target's hardware address; the advertisement that answers it
// reaches the neighbor cache.
void SolicitNeighbor(const IpAddress& target);
// For the receive task: a router advertisement or neighbor message whose checksum is correct.
void HandleNeighborDiscovery(const MacAddress& source_mac, const Ipv6Packet& packet);

} // namespace FeatherOS::Network

#endif
