#include "arp.h"

namespace FeatherOS::Network
{
namespace
{

constexpr uint16_t HardwareEthernet = 1;
constexpr uint16_t ProtocolIpv4 = 0x0800;

void ReadMac(const uint8_t* data, MacAddress* mac)
{
    for (size_t index = 0; index < sizeof(mac->bytes); ++index)
    {
        mac->bytes[index] = data[index];
    }
}

void WriteMac(uint8_t* data, const MacAddress& mac)
{
    for (size_t index = 0; index < sizeof(mac.bytes); ++index)
    {
        data[index] = mac.bytes[index];
    }
}

} // namespace

bool ParseArpPacket(const uint8_t* data, size_t length, ArpPacket* packet)
{
    if (data == nullptr || length < ArpPacketSize || ReadBig16(data) != HardwareEthernet ||
        ReadBig16(data + 2) != ProtocolIpv4 || data[4] != 6 || data[5] != 4)
    {
        return false;
    }
    packet->operation = ReadBig16(data + 6);
    ReadMac(data + 8, &packet->sender_mac);
    packet->sender_address = ReadBig32(data + 14);
    ReadMac(data + 18, &packet->target_mac);
    packet->target_address = ReadBig32(data + 24);
    return true;
}

void WriteArpPacket(uint8_t (&data)[ArpPacketSize], const ArpPacket& packet)
{
    WriteBig16(data, HardwareEthernet);
    WriteBig16(data + 2, ProtocolIpv4);
    data[4] = 6;
    data[5] = 4;
    WriteBig16(data + 6, packet.operation);
    WriteMac(data + 8, packet.sender_mac);
    WriteBig32(data + 14, packet.sender_address);
    WriteMac(data + 18, packet.target_mac);
    WriteBig32(data + 24, packet.target_address);
}

} // namespace FeatherOS::Network
