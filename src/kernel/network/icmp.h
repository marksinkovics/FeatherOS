#ifndef FEATHEROS_NETWORK_ICMP_H
#define FEATHEROS_NETWORK_ICMP_H

#include "address.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Network
{

constexpr size_t IcmpHeaderSize = 8;
constexpr uint8_t IcmpEchoReply = 0;
constexpr uint8_t IcmpDestinationUnreachable = 3;
constexpr uint8_t IcmpEchoRequest = 8;
constexpr uint8_t IcmpPortUnreachable = 3;
// An error's IPv4 packet stays within 576 bytes (RFC 1812 4.3.2.3), a 20-byte header included.
constexpr size_t MaximumIcmpErrorSize = 576 - 20;
// Errors are sent in bursts of at most this many, then one per interval (RFC 4443 2.4 f).
constexpr uint32_t IcmpErrorBurst = 10;
constexpr uint64_t IcmpErrorIntervalMs = 100;

struct IcmpEcho
{
    uint8_t type;
    uint16_t identifier;
    uint16_t sequence;
};

// What a destination unreachable error quotes of the packet that caused it: the addresses, the
// protocol and, for UDP or TCP, the ports.
struct IcmpUnreachable
{
    uint8_t code;
    IpAddress source;
    IpAddress destination;
    uint8_t protocol;
    uint16_t source_port;
    uint16_t destination_port;
};

struct IcmpErrorLimit
{
    uint32_t tokens;
    uint64_t refilled_at_ms;
};

// An echo request or reply with code 0 and a valid checksum.
bool ParseIcmpEcho(const uint8_t* message, size_t length, IcmpEcho* echo);
// Sets an echo message's type and identifier in place, keeping its sequence number and data, and
// recomputes its checksum.
void RewriteIcmpEcho(uint8_t* message, size_t length, uint8_t type, uint16_t identifier);
// Quotes as much of the IPv4 packet as fits, with a valid checksum. Returns the message's length.
size_t BuildIcmpPortUnreachable(uint8_t (&message)[MaximumIcmpErrorSize], const uint8_t* packet,
                                size_t length);
// A destination unreachable error with a valid checksum that quotes an IPv4 header and at least
// the 8 bytes after it.
bool ParseIcmpUnreachable(const uint8_t* message, size_t length, IcmpUnreachable* parsed);
// Takes one token from a bucket that starts full, or returns false when it is empty.
bool TakeIcmpErrorToken(IcmpErrorLimit* limit, uint64_t now_ms);

} // namespace FeatherOS::Network

#endif
