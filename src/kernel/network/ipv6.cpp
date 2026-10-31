#include "ipv6.h"

namespace FeatherOS::Network
{
namespace
{

constexpr uint8_t Version = 6;
constexpr uint8_t HopByHopOptions = 0;
constexpr uint8_t Routing = 43;
constexpr uint8_t Fragment = 44;
constexpr uint8_t DestinationOptions = 60;

void CopyAddress(uint8_t* destination, const IpAddress& address)
{
    for (size_t index = 0; index < sizeof(address.bytes); ++index)
    {
        destination[index] = address.bytes[index];
    }
}

IpAddress ReadAddress(const uint8_t* data)
{
    IpAddress address = Ipv6Unspecified;
    for (size_t index = 0; index < sizeof(address.bytes); ++index)
    {
        address.bytes[index] = data[index];
    }
    return address;
}

void WriteInterfaceIdentifier(IpAddress* address, const MacAddress& mac)
{
    address->bytes[8] = mac.bytes[0] ^ 0x02;
    address->bytes[9] = mac.bytes[1];
    address->bytes[10] = mac.bytes[2];
    address->bytes[11] = 0xFF;
    address->bytes[12] = 0xFE;
    address->bytes[13] = mac.bytes[3];
    address->bytes[14] = mac.bytes[4];
    address->bytes[15] = mac.bytes[5];
}

} // namespace

bool ParseIpv6Packet(const uint8_t* data, size_t length, Ipv6Packet* parsed)
{
    if (data == nullptr || parsed == nullptr || length < Ipv6HeaderSize ||
        (data[0] >> 4) != Version)
    {
        return false;
    }
    const size_t payload_length = ReadBig16(data + 4);
    if (payload_length > length - Ipv6HeaderSize)
    {
        return false;
    }
    uint8_t next_header = data[6];
    size_t offset = Ipv6HeaderSize;
    const size_t end = Ipv6HeaderSize + payload_length;
    while (next_header == HopByHopOptions || next_header == Routing ||
           next_header == DestinationOptions)
    {
        // Each of these starts with the next header and its own length in 8-byte units, not
        // counting the first 8 bytes.
        if (end - offset < 8)
        {
            return false;
        }
        const size_t header_length = (static_cast<size_t>(data[offset + 1]) + 1) * 8;
        if (header_length > end - offset)
        {
            return false;
        }
        next_header = data[offset];
        offset += header_length;
    }
    if (next_header == Fragment)
    {
        return false;
    }
    parsed->source = ReadAddress(data + 8);
    parsed->destination = ReadAddress(data + 24);
    parsed->next_header = next_header;
    parsed->hop_limit = data[7];
    parsed->payload = data + offset;
    parsed->payload_length = end - offset;
    return true;
}

size_t BuildIpv6Packet(uint8_t (&packet)[MaximumIpv6PacketSize], const Ipv6Packet& fields)
{
    if (fields.payload_length > MaximumIpv6Payload ||
        (fields.payload == nullptr && fields.payload_length != 0))
    {
        return 0;
    }
    packet[0] = Version << 4;
    packet[1] = 0;
    packet[2] = 0;
    packet[3] = 0;
    WriteBig16(packet + 4, static_cast<uint16_t>(fields.payload_length));
    packet[6] = fields.next_header;
    packet[7] = fields.hop_limit;
    CopyAddress(packet + 8, fields.source);
    CopyAddress(packet + 24, fields.destination);
    for (size_t index = 0; index < fields.payload_length; ++index)
    {
        packet[Ipv6HeaderSize + index] = fields.payload[index];
    }
    return Ipv6HeaderSize + fields.payload_length;
}

IpAddress LinkLocalAddress(const MacAddress& mac)
{
    IpAddress address = Ipv6Unspecified;
    address.bytes[0] = 0xFE;
    address.bytes[1] = 0x80;
    WriteInterfaceIdentifier(&address, mac);
    return address;
}

IpAddress PrefixAddress(const IpAddress& prefix, const MacAddress& mac)
{
    IpAddress address = Ipv6Unspecified;
    for (size_t index = 0; index < 8; ++index)
    {
        address.bytes[index] = prefix.bytes[index];
    }
    WriteInterfaceIdentifier(&address, mac);
    return address;
}

IpAddress SolicitedNodeAddress(const IpAddress& address)
{
    IpAddress result = Ipv6Unspecified;
    result.bytes[0] = 0xFF;
    result.bytes[1] = 0x02;
    result.bytes[11] = 0x01;
    result.bytes[12] = 0xFF;
    result.bytes[13] = address.bytes[13];
    result.bytes[14] = address.bytes[14];
    result.bytes[15] = address.bytes[15];
    return result;
}

MacAddress MulticastMac(const IpAddress& address)
{
    return {
        {0x33, 0x33, address.bytes[12], address.bytes[13], address.bytes[14], address.bytes[15]}};
}

bool IsLinkLocal(const IpAddress& address)
{
    return address.version == IpVersion::V6 && address.bytes[0] == 0xFE &&
           (address.bytes[1] & 0xC0) == 0x80;
}

bool IsMulticast(const IpAddress& address)
{
    return address.version == IpVersion::V6 && address.bytes[0] == 0xFF;
}

bool SamePrefix64(const IpAddress& left, const IpAddress& right)
{
    if (left.version != IpVersion::V6 || right.version != IpVersion::V6)
    {
        return false;
    }
    for (size_t index = 0; index < 8; ++index)
    {
        if (left.bytes[index] != right.bytes[index])
        {
            return false;
        }
    }
    return true;
}

} // namespace FeatherOS::Network
