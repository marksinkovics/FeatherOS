#ifndef FEATHEROS_USER_NTP_MESSAGE_H
#define FEATHEROS_USER_NTP_MESSAGE_H

#include <stddef.h>
#include <stdint.h>

// SNTP client packets (RFC 4330, NTPv4 from RFC 5905). Times outside the packet are Unix
// nanoseconds.
namespace FeatherOS::User::Ntp
{

constexpr size_t PacketSize = 48;

// An NTP timestamp holds seconds since 1900 in its top 32 bits and a binary fraction below them.
// The seconds wrap in 2036, so a value with the top bit clear is read as after the wrap: the
// timestamps cover 1968 to 2104 (RFC 4330, section 3).
uint64_t TimestampFromUnixNanoseconds(uint64_t nanoseconds);
// False before 1970.
bool UnixNanosecondsFromTimestamp(uint64_t timestamp, uint64_t* nanoseconds);

enum class ReplyStatus
{
    Valid,
    // Not a server's answer to this request.
    Malformed,
    // Stratum 0: the server refuses with the code in Reply::kiss_code, such as "RATE" or "DENY".
    Kiss,
    // The server's own clock is not synchronized.
    Unsynchronized,
};

struct Reply
{
    uint8_t stratum;
    char kiss_code[5];
    // When the server received the request and sent the reply, by its clock.
    uint64_t receive;
    uint64_t transmit;
};

struct Sample
{
    // How far the server's clock is ahead of ours.
    int64_t offset;
    // Never negative.
    int64_t round_trip_delay;
};

// A version 4 client request, carrying transmit, which the server echoes as its origin.
void BuildRequest(uint8_t (&packet)[PacketSize], uint64_t transmit);
// A server reply whose origin timestamp is the request's transmit timestamp.
ReplyStatus ParseReply(const uint8_t* packet, size_t length, uint64_t transmit, Reply* reply);
// sent and arrived are when the request left and the reply came back, by our clock.
Sample Measure(uint64_t sent, const Reply& reply, uint64_t arrived);

} // namespace FeatherOS::User::Ntp

#endif
