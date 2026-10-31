#ifndef FEATHEROS_NETWORK_NAME_SERVERS_H
#define FEATHEROS_NETWORK_NAME_SERVERS_H

#include "address.h"

#include <stddef.h>
#include <stdint.h>

// The DNS servers the C library's resolver asks. Some are configured, by DHCP or by hand, and a
// router advertisement may add one for IPv6 (RFC 8106).
namespace FeatherOS::Network
{

constexpr size_t MaximumNameServers = 3;
constexpr uint16_t DnsPort = 53;

struct NameServer
{
    IpAddress address;
    uint16_t port;
};

// Replaces the configured servers; more than MaximumNameServers are cut off.
void SetNameServers(const NameServer* servers, size_t count);
// Until the tick count reaches until; 0 forgets it.
void AdvertiseNameServer(const IpAddress& address, uint64_t until);
// The configured servers, then the advertised one while it lasts unless it is already listed.
// Returns how many it stored.
size_t ReadNameServers(NameServer (&servers)[MaximumNameServers]);

} // namespace FeatherOS::Network

#endif
