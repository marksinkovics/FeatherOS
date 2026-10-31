#include "ipv4.h"

#include "checksum.h"

namespace FeatherOS::Network
{
namespace
{

constexpr uint8_t Version = 4;
constexpr uint16_t MoreFragments = 0x2000;
constexpr uint16_t FragmentOffsetMask = 0x1FFF;

} // namespace

bool ParseIpv4Packet(const uint8_t* data, size_t length, Ipv4Packet* parsed)
{
    if (data == nullptr || parsed == nullptr || length < Ipv4HeaderSize ||
        (data[0] >> 4) != Version)
    {
        return false;
    }
    const size_t header_length = static_cast<size_t>(data[0] & 0xF) * 4;
    const size_t total_length = ReadBig16(data + 2);
    // Ethernet pads short packets, so the frame may hold more than the packet.
    if (header_length < Ipv4HeaderSize || total_length < header_length || total_length > length ||
        InternetChecksum(data, header_length) != 0)
    {
        return false;
    }
    const uint16_t fragment = ReadBig16(data + 6);
    if ((fragment & (MoreFragments | FragmentOffsetMask)) != 0)
    {
        return false;
    }
    parsed->protocol = data[9];
    parsed->source = ReadBig32(data + 12);
    parsed->destination = ReadBig32(data + 16);
    parsed->payload = data + header_length;
    parsed->payload_length = total_length - header_length;
    return true;
}

size_t BuildIpv4Packet(uint8_t (&packet)[MaximumIpv4PacketSize], const Ipv4Packet& fields,
                       uint16_t identification)
{
    if (fields.payload_length > MaximumIpv4Payload ||
        (fields.payload == nullptr && fields.payload_length != 0))
    {
        return 0;
    }
    const size_t total_length = Ipv4HeaderSize + fields.payload_length;
    packet[0] = (Version << 4) | (Ipv4HeaderSize / 4);
    packet[1] = 0;
    WriteBig16(packet + 2, static_cast<uint16_t>(total_length));
    WriteBig16(packet + 4, identification);
    WriteBig16(packet + 6, 0);
    packet[8] = DefaultTimeToLive;
    packet[9] = fields.protocol;
    WriteBig16(packet + 10, 0);
    WriteBig32(packet + 12, fields.source);
    WriteBig32(packet + 16, fields.destination);
    WriteBig16(packet + 10, InternetChecksum(packet, Ipv4HeaderSize));
    for (size_t index = 0; index < fields.payload_length; ++index)
    {
        packet[Ipv4HeaderSize + index] = fields.payload[index];
    }
    return total_length;
}

bool IsIpv4Loopback(Ipv4Address address)
{
    return (address >> 24) == 127;
}

} // namespace FeatherOS::Network
