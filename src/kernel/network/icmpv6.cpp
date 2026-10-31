#include "icmpv6.h"

namespace FeatherOS::Network
{
namespace
{

constexpr size_t NeighborMessageSize = 24;
constexpr size_t RouterAdvertisementSize = 16;
constexpr size_t RouterSolicitationSize = 8;
constexpr uint8_t SourceLinkAddressOption = 1;
constexpr uint8_t TargetLinkAddressOption = 2;
constexpr uint8_t PrefixInformationOption = 3;
constexpr uint8_t NameServerOption = 25;
constexpr size_t LinkAddressOptionSize = 8;
constexpr size_t PrefixInformationSize = 32;
// The header and at least one address.
constexpr size_t MinimumNameServerOptionSize = 24;
constexpr uint8_t RouterFlag = 0x80;
constexpr uint8_t SolicitedFlag = 0x40;
constexpr uint8_t OverrideFlag = 0x20;
constexpr uint8_t AutonomousFlag = 0x40;
constexpr size_t QuotedHeaderSize = 40;
constexpr size_t QuotedTransportSize = 8;

void ClearHeader(uint8_t* message, uint8_t type, size_t length)
{
    for (size_t index = 0; index < length; ++index)
    {
        message[index] = 0;
    }
    message[0] = type;
}

void WriteLinkAddressOption(uint8_t* option, uint8_t type, const MacAddress& mac)
{
    option[0] = type;
    option[1] = 1;
    for (size_t index = 0; index < sizeof(mac.bytes); ++index)
    {
        option[2 + index] = mac.bytes[index];
    }
}

// Each option's length counts 8-byte units, and 0 is invalid (RFC 4861 4.6). visit sees each one
// and returns false to reject the message.
template <typename Visitor> bool VisitOptions(const uint8_t* options, size_t length, Visitor visit)
{
    size_t offset = 0;
    while (offset < length)
    {
        if (length - offset < 2 || options[offset + 1] == 0)
        {
            return false;
        }
        const size_t option_length = static_cast<size_t>(options[offset + 1]) * 8;
        if (option_length > length - offset ||
            !visit(options[offset], options + offset, option_length))
        {
            return false;
        }
        offset += option_length;
    }
    return true;
}

MacAddress ReadMac(const uint8_t* data)
{
    return {{data[0], data[1], data[2], data[3], data[4], data[5]}};
}

} // namespace

bool ParseIcmpv6Echo(const uint8_t* message, size_t length, IcmpEcho* echo)
{
    if (message == nullptr || echo == nullptr || length < IcmpHeaderSize ||
        (message[0] != Icmpv6EchoRequest && message[0] != Icmpv6EchoReply) || message[1] != 0)
    {
        return false;
    }
    echo->type = message[0];
    echo->identifier = ReadBig16(message + 4);
    echo->sequence = ReadBig16(message + 6);
    return true;
}

bool ParseNeighborMessage(const uint8_t* message, size_t length, NeighborMessage* parsed)
{
    if (message == nullptr || parsed == nullptr || length < NeighborMessageSize ||
        (message[0] != NeighborSolicitation && message[0] != NeighborAdvertisement) ||
        message[1] != 0)
    {
        return false;
    }
    parsed->type = message[0];
    parsed->router = (message[4] & RouterFlag) != 0;
    parsed->solicited = (message[4] & SolicitedFlag) != 0;
    parsed->override_entry = (message[4] & OverrideFlag) != 0;
    parsed->target.version = IpVersion::V6;
    for (size_t index = 0; index < sizeof(parsed->target.bytes); ++index)
    {
        parsed->target.bytes[index] = message[8 + index];
    }
    parsed->has_link_address = false;
    const uint8_t wanted =
        parsed->type == NeighborSolicitation ? SourceLinkAddressOption : TargetLinkAddressOption;
    return VisitOptions(message + NeighborMessageSize, length - NeighborMessageSize,
                        [&](uint8_t type, const uint8_t* option, size_t option_length)
                        {
                            if (type == wanted && option_length == LinkAddressOptionSize)
                            {
                                parsed->has_link_address = true;
                                parsed->link_address = ReadMac(option + 2);
                            }
                            return true;
                        });
}

bool ParseRouterAdvertisement(const uint8_t* message, size_t length, RouterAdvertisement* parsed)
{
    if (message == nullptr || parsed == nullptr || length < RouterAdvertisementSize ||
        message[0] != RouterAdvertisementType || message[1] != 0)
    {
        return false;
    }
    parsed->router_lifetime = ReadBig16(message + 6);
    parsed->has_link_address = false;
    parsed->has_prefix = false;
    parsed->has_name_server = false;
    return VisitOptions(
        message + RouterAdvertisementSize, length - RouterAdvertisementSize,
        [&](uint8_t type, const uint8_t* option, size_t option_length)
        {
            if (type == SourceLinkAddressOption && option_length == LinkAddressOptionSize)
            {
                parsed->has_link_address = true;
                parsed->link_address = ReadMac(option + 2);
            }
            else if (type == PrefixInformationOption && option_length == PrefixInformationSize &&
                     !parsed->has_prefix && (option[3] & AutonomousFlag) != 0)
            {
                parsed->has_prefix = true;
                parsed->prefix_length = option[2];
                parsed->valid_lifetime = ReadBig32(option + 4);
                parsed->prefix.version = IpVersion::V6;
                for (size_t index = 0; index < sizeof(parsed->prefix.bytes); ++index)
                {
                    parsed->prefix.bytes[index] = option[16 + index];
                }
            }
            else if (type == NameServerOption && option_length >= MinimumNameServerOptionSize &&
                     !parsed->has_name_server)
            {
                parsed->has_name_server = true;
                parsed->name_server_lifetime = ReadBig32(option + 4);
                parsed->name_server.version = IpVersion::V6;
                for (size_t index = 0; index < sizeof(parsed->name_server.bytes); ++index)
                {
                    parsed->name_server.bytes[index] = option[8 + index];
                }
            }
            return true;
        });
}

size_t BuildNeighborSolicitation(uint8_t (&message)[MaximumNeighborMessageSize],
                                 const IpAddress& target, const MacAddress* source_link)
{
    ClearHeader(message, NeighborSolicitation, NeighborMessageSize);
    for (size_t index = 0; index < sizeof(target.bytes); ++index)
    {
        message[8 + index] = target.bytes[index];
    }
    if (source_link == nullptr)
    {
        return NeighborMessageSize;
    }
    WriteLinkAddressOption(message + NeighborMessageSize, SourceLinkAddressOption, *source_link);
    return NeighborMessageSize + LinkAddressOptionSize;
}

size_t BuildNeighborAdvertisement(uint8_t (&message)[MaximumNeighborMessageSize],
                                  const IpAddress& target, const MacAddress& link, bool solicited)
{
    ClearHeader(message, NeighborAdvertisement, NeighborMessageSize);
    message[4] = OverrideFlag | (solicited ? SolicitedFlag : 0);
    for (size_t index = 0; index < sizeof(target.bytes); ++index)
    {
        message[8 + index] = target.bytes[index];
    }
    WriteLinkAddressOption(message + NeighborMessageSize, TargetLinkAddressOption, link);
    return NeighborMessageSize + LinkAddressOptionSize;
}

size_t BuildRouterSolicitation(uint8_t (&message)[MaximumNeighborMessageSize],
                               const MacAddress* source_link)
{
    ClearHeader(message, RouterSolicitation, RouterSolicitationSize);
    if (source_link == nullptr)
    {
        return RouterSolicitationSize;
    }
    WriteLinkAddressOption(message + RouterSolicitationSize, SourceLinkAddressOption, *source_link);
    return RouterSolicitationSize + LinkAddressOptionSize;
}

size_t BuildIcmpv6PortUnreachable(uint8_t (&message)[MaximumIcmpv6ErrorSize], const uint8_t* packet,
                                  size_t length)
{
    const size_t room = MaximumIcmpv6ErrorSize - IcmpHeaderSize;
    const size_t quoted = length < room ? length : room;
    ClearHeader(message, Icmpv6DestinationUnreachable, IcmpHeaderSize);
    message[1] = Icmpv6PortUnreachable;
    for (size_t index = 0; index < quoted; ++index)
    {
        message[IcmpHeaderSize + index] = packet[index];
    }
    return IcmpHeaderSize + quoted;
}

bool ParseIcmpv6Unreachable(const uint8_t* message, size_t length, IcmpUnreachable* parsed)
{
    if (message == nullptr || parsed == nullptr ||
        length < IcmpHeaderSize + QuotedHeaderSize + QuotedTransportSize ||
        message[0] != Icmpv6DestinationUnreachable)
    {
        return false;
    }
    const uint8_t* quoted = message + IcmpHeaderSize;
    if ((quoted[0] >> 4) != 6)
    {
        return false;
    }
    parsed->code = message[1];
    parsed->protocol = quoted[6];
    parsed->source.version = IpVersion::V6;
    parsed->destination.version = IpVersion::V6;
    for (size_t index = 0; index < sizeof(parsed->source.bytes); ++index)
    {
        parsed->source.bytes[index] = quoted[8 + index];
        parsed->destination.bytes[index] = quoted[24 + index];
    }
    parsed->source_port = ReadBig16(quoted + QuotedHeaderSize);
    parsed->destination_port = ReadBig16(quoted + QuotedHeaderSize + 2);
    return true;
}

} // namespace FeatherOS::Network
