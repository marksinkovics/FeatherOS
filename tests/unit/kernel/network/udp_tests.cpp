#include <catch2/catch_test_macros.hpp>

#include "kernel/network/address.h"
#include "kernel/network/udp.h"

#include <vector>

using namespace FeatherOS::Network;

TEST_CASE("a UDP header is written in front of its payload")
{
    std::vector<uint8_t> data(UdpHeaderSize + 3, 0xEE);
    WriteUdpHeader(data.data(), 49152, 53, 3);
    REQUIRE(data == std::vector<uint8_t>{0xC0, 0x00, 0x00, 0x35, 0x00, 0x0B, 0x00, 0x00, 0xEE,
                                         0xEE, 0xEE});

    UdpDatagram datagram{};
    REQUIRE(ParseUdpDatagram(data.data(), data.size(), &datagram));
    REQUIRE(datagram.source_port == 49152);
    REQUIRE(datagram.destination_port == 53);
    REQUIRE(datagram.checksum == 0);
    REQUIRE(datagram.payload == data.data() + UdpHeaderSize);
    REQUIRE(datagram.payload_length == 3);
}

TEST_CASE("the UDP length decides the payload, not padding after it")
{
    std::vector<uint8_t> data(UdpHeaderSize + 6, 0);
    WriteUdpHeader(data.data(), 67, 68, 2);
    data[UdpChecksumOffset] = 0x12;
    UdpDatagram datagram{};
    REQUIRE(ParseUdpDatagram(data.data(), data.size(), &datagram));
    REQUIRE(datagram.payload_length == 2);
    REQUIRE(datagram.checksum == 0x1200);
}

TEST_CASE("a UDP length outside the packet is refused")
{
    std::vector<uint8_t> data(UdpHeaderSize + 2, 0);
    UdpDatagram datagram{};
    WriteBig16(data.data() + 4, static_cast<uint16_t>(data.size() + 1));
    REQUIRE_FALSE(ParseUdpDatagram(data.data(), data.size(), &datagram));
    WriteBig16(data.data() + 4, UdpHeaderSize - 1);
    REQUIRE_FALSE(ParseUdpDatagram(data.data(), data.size(), &datagram));
    REQUIRE_FALSE(ParseUdpDatagram(data.data(), UdpHeaderSize - 1, &datagram));
}
