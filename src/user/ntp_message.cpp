#include "ntp_message.h"

namespace FeatherOS::User::Ntp
{
namespace
{

constexpr uint64_t NanosecondsPerSecond = 1000000000;
// From 1900-01-01 to 1970-01-01.
constexpr uint64_t UnixEpochSeconds = 2208988800;
constexpr uint64_t EraSeconds = 1ULL << 32;
constexpr uint8_t Version = 4;
constexpr uint8_t ModeClient = 3;
constexpr uint8_t ModeServer = 4;
constexpr uint8_t LeapUnsynchronized = 3;
constexpr uint8_t MaximumStratum = 15;
constexpr size_t ReferenceIdOffset = 12;
constexpr size_t OriginOffset = 24;
constexpr size_t ReceiveOffset = 32;
constexpr size_t TransmitOffset = 40;

uint64_t Read64(const uint8_t* bytes)
{
    uint64_t value = 0;
    for (size_t index = 0; index < 8; ++index)
    {
        value = value << 8 | bytes[index];
    }
    return value;
}

void Write64(uint8_t* bytes, uint64_t value)
{
    for (size_t index = 0; index < 8; ++index)
    {
        bytes[7 - index] = static_cast<uint8_t>(value >> (index * 8));
    }
}

int64_t Difference(uint64_t later, uint64_t earlier)
{
    return static_cast<int64_t>(later) - static_cast<int64_t>(earlier);
}

} // namespace

uint64_t TimestampFromUnixNanoseconds(uint64_t nanoseconds)
{
    const uint64_t seconds = (nanoseconds / NanosecondsPerSecond + UnixEpochSeconds) % EraSeconds;
    const uint64_t remainder = nanoseconds % NanosecondsPerSecond;
    const uint64_t fraction = ((remainder << 32) + NanosecondsPerSecond / 2) / NanosecondsPerSecond;
    return seconds << 32 | fraction;
}

bool UnixNanosecondsFromTimestamp(uint64_t timestamp, uint64_t* nanoseconds)
{
    uint64_t seconds = timestamp >> 32;
    if ((seconds & 0x80000000) == 0)
    {
        seconds += EraSeconds;
    }
    if (seconds < UnixEpochSeconds)
    {
        return false;
    }
    const uint64_t fraction = timestamp & 0xFFFFFFFF;
    *nanoseconds = (seconds - UnixEpochSeconds) * NanosecondsPerSecond +
                   ((fraction * NanosecondsPerSecond + (1ULL << 31)) >> 32);
    return true;
}

void BuildRequest(uint8_t (&packet)[PacketSize], uint64_t transmit)
{
    for (uint8_t& byte : packet)
    {
        byte = 0;
    }
    packet[0] = Version << 3 | ModeClient;
    Write64(packet + TransmitOffset, transmit);
}

ReplyStatus ParseReply(const uint8_t* packet, size_t length, uint64_t transmit, Reply* reply)
{
    if (packet == nullptr || length < PacketSize)
    {
        return ReplyStatus::Malformed;
    }
    const uint8_t leap = packet[0] >> 6;
    const uint8_t version = (packet[0] >> 3) & 7;
    const uint8_t mode = packet[0] & 7;
    // A reply that does not echo our transmit timestamp answers another request, or none.
    if (mode != ModeServer || version < 3 || version > Version ||
        Read64(packet + OriginOffset) != transmit)
    {
        return ReplyStatus::Malformed;
    }
    reply->stratum = packet[1];
    for (size_t index = 0; index < 4; ++index)
    {
        const uint8_t byte = packet[ReferenceIdOffset + index];
        reply->kiss_code[index] = byte >= ' ' && byte <= '~' ? static_cast<char>(byte) : '?';
    }
    reply->kiss_code[4] = '\0';
    // A kiss may leave its other timestamps 0.
    if (reply->stratum == 0)
    {
        return ReplyStatus::Kiss;
    }
    if (Read64(packet + TransmitOffset) == 0)
    {
        return ReplyStatus::Malformed;
    }
    if (leap == LeapUnsynchronized || reply->stratum > MaximumStratum)
    {
        return ReplyStatus::Unsynchronized;
    }
    if (!UnixNanosecondsFromTimestamp(Read64(packet + ReceiveOffset), &reply->receive) ||
        !UnixNanosecondsFromTimestamp(Read64(packet + TransmitOffset), &reply->transmit))
    {
        return ReplyStatus::Malformed;
    }
    return ReplyStatus::Valid;
}

Sample Measure(uint64_t sent, const Reply& reply, uint64_t arrived)
{
    const int64_t outbound = Difference(reply.receive, sent);
    const int64_t inbound = Difference(reply.transmit, arrived);
    // A round trip shorter than our clock's tick measures as less than the server's own delay.
    const int64_t delay = Difference(arrived, sent) - Difference(reply.transmit, reply.receive);
    return {outbound / 2 + inbound / 2 + (outbound % 2 + inbound % 2) / 2, delay < 0 ? 0 : delay};
}

} // namespace FeatherOS::User::Ntp
