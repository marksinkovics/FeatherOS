#ifndef FEATHEROS_LIBC_DNS_MESSAGE_H
#define FEATHEROS_LIBC_DNS_MESSAGE_H

#include <stddef.h>
#include <stdint.h>

// DNS queries and responses (RFC 1035), for getaddrinfo and getnameinfo.
namespace FeatherOS::LibC::Dns
{

constexpr uint16_t TypeA = 1;
constexpr uint16_t TypeAaaa = 28;
constexpr uint16_t TypePtr = 12;
// Without EDNS, a UDP response holds at most 512 bytes.
constexpr size_t MaximumMessageSize = 512;
constexpr size_t MaximumAddresses = 8;
// A domain name of 253 bytes and its terminator.
constexpr size_t NameCapacity = 254;

enum class Outcome : uint8_t
{
    // Not a response to this query; keep waiting.
    Unrelated,
    // Addresses of the asked type, possibly none when the name has only other records.
    Answered,
    // The name does not exist.
    NoName,
    // The server failed or refused; another one may answer.
    Failed,
    // The answer did not fit in a datagram; ask again over TCP (RFC 7766).
    Truncated,
};

struct Addresses
{
    size_t count;
    // 4 or 16 bytes each, in network order.
    uint8_t bytes[MaximumAddresses][16];
};

// A recursive query for one record type. Returns the message's length, or 0 when the name is not
// a valid domain name (labels of 1 to 63 bytes, 253 in all, an optional final dot) or does not
// fit.
size_t BuildQuery(uint8_t* message, size_t capacity, uint16_t id, const char* name, uint16_t type);
// Reads a response to the query with this ID and type, collecting the answer records of the type
// whatever their owner name, which covers answers that follow a CNAME.
Outcome ParseResponse(const uint8_t* message, size_t length, uint16_t id, uint16_t type,
                      Addresses* addresses);
// The name whose PTR records name a 4- or 16-byte address: "2.2.0.10.in-addr.arpa", or 32
// reversed hex digits under "ip6.arpa" (RFC 3596 2.5). Returns its length, or 0 when it does not
// fit.
size_t BuildReverseName(const uint8_t* address, size_t address_length, char* name, size_t capacity);
// ParseResponse for a PTR query: the first PTR record's name without a final dot, or an empty
// name when there is none. Names that do not fit, or hold characters other than letters, digits,
// '-' and '_', are skipped, so nothing else reaches a terminal.
Outcome ParsePointerResponse(const uint8_t* message, size_t length, uint16_t id, char* name,
                             size_t capacity);

} // namespace FeatherOS::LibC::Dns

#endif
