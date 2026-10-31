#ifndef FEATHEROS_NETWORK_INTERFACE_H
#define FEATHEROS_NETWORK_INTERFACE_H

#include "address.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Network
{

struct InterfaceConfiguration
{
    Ipv4Address address;
    Ipv4Address netmask;
    Ipv4Address gateway;
};

// Starts the device and the task that handles received frames; needs the scheduler and timer.
bool Initialize(const void* platform_data, uint64_t hhdm_offset);
// True when a device was found, whether its link is up or not.
bool IsUp();
bool LinkIsUp();
// The link is up and an address has a way off the local network: IPv4 with a gateway, or a global
// IPv6 address with a router. Whether that path reaches the internet is not tested.
bool HasRoute();
// Down, the device is reset and the IPv4 and IPv6 addresses, the router's name server, the
// neighbor cache and TCP connections to other hosts are dropped, so only loopback works. Up, the
// device starts again and so does IPv6 autoconfiguration, waiting up to a second for a router;
// IPv4 waits for DHCP or an address set by hand. False when the device does not start.
bool SetLink(bool link_up);
MacAddress LocalMac();
// The IPv4 address is 0 until it is set.
InterfaceConfiguration Configuration();
// False, changing nothing, while the link is down.
bool SetConfiguration(const InterfaceConfiguration& replacement);
// Asks for the hardware address of a host on the local network, through ARP for IPv4 and
// neighbor discovery for IPv6, retrying a few times.
bool Resolve(const IpAddress& address, MacAddress* mac);
// Records a neighbor's hardware address. A host not known yet is only added when learn_new is
// set, such as when it asked for one of our addresses (RFC 826, RFC 4861 7.2.3).
void LearnNeighbor(const IpAddress& address, const MacAddress& mac, bool learn_new);
bool SendFrame(const MacAddress& destination, uint16_t type, const uint8_t* payload, size_t length);

} // namespace FeatherOS::Network

#endif
