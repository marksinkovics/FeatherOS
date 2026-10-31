#include <catch2/catch_test_macros.hpp>

#include "kernel/network/arp.h"

using namespace FeatherOS::Network;

namespace
{

constexpr MacAddress Local = {{0x52, 0x54, 0x00, 0x12, 0x34, 0x56}};
constexpr MacAddress Gateway = {{0x52, 0x55, 0x0A, 0x00, 0x02, 0x02}};

} // namespace

TEST_CASE("an ARP request round-trips through its wire format")
{
    uint8_t data[ArpPacketSize];
    WriteArpPacket(data, {
                             .operation = ArpRequest,
                             .sender_mac = Local,
                             .sender_address = MakeIpv4(10, 0, 2, 15),
                             .target_mac = {},
                             .target_address = MakeIpv4(10, 0, 2, 2),
                         });
    // Hardware type 1 (Ethernet), protocol 0x0800 (IPv4), address lengths 6 and 4.
    REQUIRE(data[1] == 1);
    REQUIRE(data[2] == 0x08);
    REQUIRE(data[4] == 6);
    REQUIRE(data[5] == 4);
    REQUIRE(data[7] == ArpRequest);
    REQUIRE(data[14] == 10);
    REQUIRE(data[27] == 2);

    ArpPacket packet;
    REQUIRE(ParseArpPacket(data, sizeof(data), &packet));
    REQUIRE(packet.operation == ArpRequest);
    REQUIRE(packet.sender_mac == Local);
    REQUIRE(packet.sender_address == MakeIpv4(10, 0, 2, 15));
    REQUIRE(packet.target_mac == MacAddress{});
    REQUIRE(packet.target_address == MakeIpv4(10, 0, 2, 2));
}

TEST_CASE("ARP packets for other hardware or protocols are rejected")
{
    uint8_t data[ArpPacketSize];
    WriteArpPacket(data, {.operation = ArpReply,
                          .sender_mac = Gateway,
                          .sender_address = MakeIpv4(10, 0, 2, 2),
                          .target_mac = Local,
                          .target_address = MakeIpv4(10, 0, 2, 15)});
    ArpPacket packet;
    REQUIRE_FALSE(ParseArpPacket(data, ArpPacketSize - 1, &packet));
    data[3] = 0xDD;
    REQUIRE_FALSE(ParseArpPacket(data, sizeof(data), &packet));
    data[3] = 0x00;
    data[4] = 8;
    REQUIRE_FALSE(ParseArpPacket(data, sizeof(data), &packet));
}
