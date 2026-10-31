#ifndef FEATHEROS_NETWORK_IP_H
#define FEATHEROS_NETWORK_IP_H

#include "address.h"

#include <stddef.h>
#include <stdint.h>

// IP input and output above the interface.
namespace FeatherOS::Network
{

enum class SendStatus : uint8_t
{
    Sent,
    // No interface, or none for the destination's IP version.
    NoRoute,
    // The link is down and the destination is not a loopback address.
    NetworkDown,
    // The next hop did not answer address resolution.
    HostUnreachable,
    TooLong,
    // Every transmit buffer is in use.
    NoBuffers,
};

constexpr size_t NoChecksum = SIZE_MAX;

// Sends payload from the source address that suits the destination, or from the destination
// itself for loopback, in which case the packet is handled at once without reaching the device.
// Off the local network it goes through the gateway or router. With a checksum_offset, the
// 16-bit field there receives the checksum over the payload and the pseudo-header of the chosen
// source, as UDP, TCP and ICMPv6 need. Resolving the next hop can wait a few seconds, so the
// receive task never sends through here.
SendStatus SendIp(const IpAddress& destination, uint8_t protocol, uint8_t* payload, size_t length,
                  size_t checksum_offset = NoChecksum);
// One of our addresses, or a loopback one.
bool IsLocalAddress(const IpAddress& address);
// The address SendIp sends from to destination; unspecified when there is none, such as before
// DHCP set an IPv4 address.
IpAddress SourceAddressFor(const IpAddress& destination);
// For the receive task: an IPv4 or IPv6 packet in a frame from source.
void ReceiveIpv4(const MacAddress& source, const uint8_t* packet, size_t length);
void ReceiveIpv6(const MacAddress& source, const uint8_t* packet, size_t length);
// Sends an ICMPv6 message straight to a hardware address, without routing or address
// resolution, filling in its checksum.
bool SendIcmpv6(const MacAddress& destination_mac, const IpAddress& source,
                const IpAddress& destination, uint8_t hop_limit, uint8_t* message, size_t length);

} // namespace FeatherOS::Network

#endif
