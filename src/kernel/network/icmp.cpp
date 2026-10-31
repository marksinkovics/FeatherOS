#include "icmp.h"

#include "address.h"
#include "checksum.h"

namespace FeatherOS::Network
{
namespace
{

constexpr size_t QuotedTransportSize = 8;

} // namespace

bool ParseIcmpEcho(const uint8_t* message, size_t length, IcmpEcho* echo)
{
    if (message == nullptr || echo == nullptr || length < IcmpHeaderSize ||
        (message[0] != IcmpEchoRequest && message[0] != IcmpEchoReply) || message[1] != 0 ||
        InternetChecksum(message, length) != 0)
    {
        return false;
    }
    echo->type = message[0];
    echo->identifier = ReadBig16(message + 4);
    echo->sequence = ReadBig16(message + 6);
    return true;
}

void RewriteIcmpEcho(uint8_t* message, size_t length, uint8_t type, uint16_t identifier)
{
    message[0] = type;
    message[1] = 0;
    WriteBig16(message + 2, 0);
    WriteBig16(message + 4, identifier);
    WriteBig16(message + 2, InternetChecksum(message, length));
}

size_t BuildIcmpPortUnreachable(uint8_t (&message)[MaximumIcmpErrorSize], const uint8_t* packet,
                                size_t length)
{
    const size_t quoted = length < MaximumIcmpErrorSize - IcmpHeaderSize
                              ? length
                              : MaximumIcmpErrorSize - IcmpHeaderSize;
    message[0] = IcmpDestinationUnreachable;
    message[1] = IcmpPortUnreachable;
    WriteBig16(message + 2, 0);
    WriteBig32(message + 4, 0);
    for (size_t index = 0; index < quoted; ++index)
    {
        message[IcmpHeaderSize + index] = packet[index];
    }
    WriteBig16(message + 2, InternetChecksum(message, IcmpHeaderSize + quoted));
    return IcmpHeaderSize + quoted;
}

bool ParseIcmpUnreachable(const uint8_t* message, size_t length, IcmpUnreachable* parsed)
{
    constexpr size_t MinimumHeaderSize = 20;
    if (message == nullptr || parsed == nullptr ||
        length < IcmpHeaderSize + MinimumHeaderSize + QuotedTransportSize ||
        message[0] != IcmpDestinationUnreachable || InternetChecksum(message, length) != 0)
    {
        return false;
    }
    const uint8_t* quoted = message + IcmpHeaderSize;
    const size_t header_length = static_cast<size_t>(quoted[0] & 0xF) * 4;
    if ((quoted[0] >> 4) != 4 || header_length < MinimumHeaderSize ||
        header_length + QuotedTransportSize > length - IcmpHeaderSize)
    {
        return false;
    }
    parsed->code = message[1];
    parsed->protocol = quoted[9];
    parsed->source = MakeIpAddress(ReadBig32(quoted + 12));
    parsed->destination = MakeIpAddress(ReadBig32(quoted + 16));
    parsed->source_port = ReadBig16(quoted + header_length);
    parsed->destination_port = ReadBig16(quoted + header_length + 2);
    return true;
}

bool TakeIcmpErrorToken(IcmpErrorLimit* limit, uint64_t now_ms)
{
    if (now_ms > limit->refilled_at_ms)
    {
        const uint64_t added = (now_ms - limit->refilled_at_ms) / IcmpErrorIntervalMs;
        if (limit->tokens + added >= IcmpErrorBurst)
        {
            limit->tokens = IcmpErrorBurst;
            limit->refilled_at_ms = now_ms;
        }
        else
        {
            limit->tokens += static_cast<uint32_t>(added);
            limit->refilled_at_ms += added * IcmpErrorIntervalMs;
        }
    }
    if (limit->tokens == 0)
    {
        return false;
    }
    --limit->tokens;
    return true;
}

} // namespace FeatherOS::Network
