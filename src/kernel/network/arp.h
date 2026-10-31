#ifndef FEATHEROS_NETWORK_ARP_H
#define FEATHEROS_NETWORK_ARP_H

#include "address.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Network
{

// An Ethernet/IPv4 ARP packet; other hardware or protocol types are not parsed.
constexpr size_t ArpPacketSize = 28;
constexpr uint16_t ArpRequest = 1;
constexpr uint16_t ArpReply = 2;

struct ArpPacket
{
    uint16_t operation;
    MacAddress sender_mac;
    Ipv4Address sender_address;
    MacAddress target_mac;
    Ipv4Address target_address;
};

bool ParseArpPacket(const uint8_t* data, size_t length, ArpPacket* packet);
void WriteArpPacket(uint8_t (&data)[ArpPacketSize], const ArpPacket& packet);

} // namespace FeatherOS::Network

#endif
