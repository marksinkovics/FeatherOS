#ifndef FEATHEROS_LIBC_ADDRESS_TEXT_H
#define FEATHEROS_LIBC_ADDRESS_TEXT_H

#include <stddef.h>
#include <stdint.h>

// Network addresses as text, behind inet_pton and inet_ntop.
namespace FeatherOS::LibC
{

constexpr size_t Ipv4TextCapacity = 16;
constexpr size_t Ipv6TextCapacity = 46;

// Exactly four dot-separated decimal parts from 0 to 255, without leading zeros, as inet_pton
// requires; the address is in network order.
bool ParseIpv4Text(const char* text, uint8_t (&address)[4]);
// Writes "10.0.2.15" and its terminator. Returns the length without the terminator, or 0 when
// capacity is too small.
size_t FormatIpv4Text(const uint8_t (&address)[4], char* text, size_t capacity);
// Eight groups of one to four hex digits, at most one "::" standing for one or more zero groups,
// and optionally a dotted IPv4 address as the last 32 bits (RFC 4291 2.2). No zone suffix.
bool ParseIpv6Text(const char* text, uint8_t (&address)[16]);
// Writes the RFC 5952 form, such as "fe80::5054:ff:fe12:3456", like FormatIpv4Text.
size_t FormatIpv6Text(const uint8_t (&address)[16], char* text, size_t capacity);
// Either version, from length characters that need no terminator. Returns 4 or 16 for the bytes
// written, or 0 when the text is neither.
size_t ParseAddressText(const char* text, size_t length, uint8_t (&address)[16]);

} // namespace FeatherOS::LibC

#endif
