#ifndef FEATHEROS_TESTS_NETWORK_STUBS_H
#define FEATHEROS_TESTS_NETWORK_STUBS_H

#include "kernel/network/address.h"
#include "kernel/network/ip.h"

#include <stddef.h>
#include <stdint.h>

#include <vector>

// Stand-ins for the IP layer under the socket code: sent packets are recorded, and each send
// reports `status`. With link_up false, only loopback destinations have a source address.
namespace TestNetwork
{

struct SentPacket
{
    FeatherOS::Network::IpAddress destination;
    uint8_t protocol;
    std::vector<uint8_t> payload;
    size_t checksum_offset;
};

extern std::vector<SentPacket> sent;
extern FeatherOS::Network::SendStatus status;
extern FeatherOS::Network::IpAddress local_address;
// What SourceAddressFor picks for IPv6.
extern FeatherOS::Network::IpAddress local_ipv6_address;
extern bool link_up;

void Reset();

} // namespace TestNetwork

#endif
