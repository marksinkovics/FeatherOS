#ifndef FEATHEROS_NETWORK_ADDRESS_H
#define FEATHEROS_NETWORK_ADDRESS_H

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Network
{

struct MacAddress
{
    uint8_t bytes[6];
};

// In host byte order, so 10.0.2.15 is 0x0A00020F.
using Ipv4Address = uint32_t;

enum class IpVersion : uint8_t
{
    None,
    V4,
    V6,
};

// An address of either IP version, in network byte order; an IPv4 address fills the first four
// bytes and leaves the rest zero.
struct IpAddress
{
    IpVersion version;
    uint8_t bytes[16];
};

constexpr MacAddress BroadcastMac = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
constexpr size_t FormattedMacLength = 18;
constexpr size_t FormattedIpv4Length = 16;
constexpr size_t FormattedIpv6Length = 46;

constexpr Ipv4Address MakeIpv4(uint8_t first, uint8_t second, uint8_t third, uint8_t fourth)
{
    return (static_cast<uint32_t>(first) << 24) | (static_cast<uint32_t>(second) << 16) |
           (static_cast<uint32_t>(third) << 8) | fourth;
}

bool operator==(const MacAddress& left, const MacAddress& right);
bool operator==(const IpAddress& left, const IpAddress& right);
IpAddress MakeIpAddress(Ipv4Address address);
// The IPv4 address held, or 0 for an address of another version.
Ipv4Address ToIpv4(const IpAddress& address);
// True for an address of the version whose bytes are all zero, such as 0.0.0.0.
bool IsUnspecified(const IpAddress& address);
// The number of leading one bits, so 255.255.255.0 gives 24.
uint32_t PrefixLength(Ipv4Address netmask);
// The reverse, for lengths up to 32.
Ipv4Address PrefixNetmask(uint32_t length);
// Writes "52:54:00:12:34:56" with its terminator.
void FormatMac(const MacAddress& address, char (&text)[FormattedMacLength]);
// Writes "10.0.2.15" with its terminator.
void FormatIpv4(Ipv4Address address, char (&text)[FormattedIpv4Length]);
// Writes the RFC 5952 form, such as "fe80::5054:ff:fe12:3456", with its terminator.
void FormatIpv6(const IpAddress& address, char (&text)[FormattedIpv6Length]);

uint16_t ReadBig16(const uint8_t* data);
uint32_t ReadBig32(const uint8_t* data);
void WriteBig16(uint8_t* data, uint16_t value);
void WriteBig32(uint8_t* data, uint32_t value);

} // namespace FeatherOS::Network

#endif
