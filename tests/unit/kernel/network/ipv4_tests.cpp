#include <catch2/catch_test_macros.hpp>

#include "kernel/network/checksum.h"
#include "kernel/network/ipv4.h"

using namespace FeatherOS::Network;

namespace
{

// A UDP packet's header from 192.168.0.1 to 192.168.0.199 whose checksum is 0xB861.
uint8_t sample_header[] = {0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00, 0x40, 0x11,
                           0xB8, 0x61, 0xC0, 0xA8, 0x00, 0x01, 0xC0, 0xA8, 0x00, 0xC7};

size_t BuildSample(uint8_t (&packet)[MaximumIpv4PacketSize])
{
    const uint8_t payload[] = {'p', 'i', 'n', 'g'};
    return BuildIpv4Packet(packet,
                           {.source = MakeIpv4(10, 0, 2, 15),
                            .destination = MakeIpv4(10, 0, 2, 2),
                            .protocol = IpProtocolIcmp,
                            .payload = payload,
                            .payload_length = sizeof(payload)},
                           0x1234);
}

void FixChecksum(uint8_t* packet)
{
    packet[10] = 0;
    packet[11] = 0;
    const uint16_t checksum = InternetChecksum(packet, static_cast<size_t>(packet[0] & 0xF) * 4);
    packet[10] = static_cast<uint8_t>(checksum >> 8);
    packet[11] = static_cast<uint8_t>(checksum);
}

} // namespace

TEST_CASE("a known IPv4 header has a valid checksum")
{
    REQUIRE(InternetChecksum(sample_header, sizeof(sample_header)) == 0);
}

TEST_CASE("an IPv4 packet round-trips through its wire format")
{
    uint8_t packet[MaximumIpv4PacketSize];
    const size_t length = BuildSample(packet);
    REQUIRE(length == Ipv4HeaderSize + 4);
    REQUIRE(packet[0] == 0x45);
    REQUIRE(ReadBig16(packet + 2) == length);
    REQUIRE(ReadBig16(packet + 4) == 0x1234);
    REQUIRE(packet[8] == DefaultTimeToLive);
    REQUIRE(InternetChecksum(packet, Ipv4HeaderSize) == 0);

    Ipv4Packet parsed;
    REQUIRE(ParseIpv4Packet(packet, length, &parsed));
    REQUIRE(parsed.source == MakeIpv4(10, 0, 2, 15));
    REQUIRE(parsed.destination == MakeIpv4(10, 0, 2, 2));
    REQUIRE(parsed.protocol == IpProtocolIcmp);
    REQUIRE(parsed.payload == packet + Ipv4HeaderSize);
    REQUIRE(parsed.payload_length == 4);
    REQUIRE(parsed.payload[0] == 'p');
}

TEST_CASE("an IPv4 packet in a padded frame ends at its total length")
{
    uint8_t packet[MaximumIpv4PacketSize] = {};
    const size_t length = BuildSample(packet);
    Ipv4Packet parsed;
    REQUIRE(ParseIpv4Packet(packet, length + 22, &parsed));
    REQUIRE(parsed.payload_length == 4);
}

TEST_CASE("IPv4 options are skipped")
{
    uint8_t packet[MaximumIpv4PacketSize] = {};
    BuildSample(packet);
    // Move the payload up four bytes to make room for one word of options (all no-ops).
    for (size_t index = 4; index-- > 0;)
    {
        packet[Ipv4HeaderSize + 4 + index] = packet[Ipv4HeaderSize + index];
    }
    for (size_t index = 0; index < 4; ++index)
    {
        packet[Ipv4HeaderSize + index] = 1;
    }
    packet[0] = 0x46;
    WriteBig16(packet + 2, Ipv4HeaderSize + 8);
    FixChecksum(packet);
    Ipv4Packet parsed;
    REQUIRE(ParseIpv4Packet(packet, Ipv4HeaderSize + 8, &parsed));
    REQUIRE(parsed.payload == packet + Ipv4HeaderSize + 4);
    REQUIRE(parsed.payload_length == 4);
    REQUIRE(parsed.payload[0] == 'p');
}

TEST_CASE("malformed IPv4 packets are rejected")
{
    uint8_t packet[MaximumIpv4PacketSize];
    const size_t length = BuildSample(packet);
    Ipv4Packet parsed;
    REQUIRE_FALSE(ParseIpv4Packet(packet, Ipv4HeaderSize - 1, &parsed));
    REQUIRE_FALSE(ParseIpv4Packet(packet, length - 1, &parsed));

    SECTION("a bad checksum")
    {
        packet[8] = 1;
    }
    SECTION("another version")
    {
        packet[0] = 0x65;
        FixChecksum(packet);
    }
    SECTION("a header shorter than five words")
    {
        packet[0] = 0x44;
        FixChecksum(packet);
    }
    SECTION("a total length shorter than the header")
    {
        WriteBig16(packet + 2, Ipv4HeaderSize - 1);
        FixChecksum(packet);
    }
    REQUIRE_FALSE(ParseIpv4Packet(packet, length, &parsed));
}

TEST_CASE("IPv4 fragments are dropped")
{
    uint8_t packet[MaximumIpv4PacketSize];
    const size_t length = BuildSample(packet);
    Ipv4Packet parsed;
    // Don't Fragment alone is fine.
    WriteBig16(packet + 6, 0x4000);
    FixChecksum(packet);
    REQUIRE(ParseIpv4Packet(packet, length, &parsed));

    SECTION("the first fragment, with More Fragments set")
    {
        WriteBig16(packet + 6, 0x2000);
    }
    SECTION("a later fragment, with an offset")
    {
        WriteBig16(packet + 6, 0x0010);
    }
    FixChecksum(packet);
    REQUIRE_FALSE(ParseIpv4Packet(packet, length, &parsed));
}

TEST_CASE("an IPv4 payload that does not fit is refused")
{
    static uint8_t payload[MaximumIpv4Payload + 1];
    uint8_t packet[MaximumIpv4PacketSize];
    const Ipv4Packet fields = {.source = 1,
                               .destination = 2,
                               .protocol = IpProtocolIcmp,
                               .payload = payload,
                               .payload_length = sizeof(payload)};
    REQUIRE(BuildIpv4Packet(packet, fields, 0) == 0);
    REQUIRE(BuildIpv4Packet(packet, {fields.source, fields.destination, fields.protocol, payload,
                                     MaximumIpv4Payload},
                            0) == MaximumIpv4PacketSize);
}

TEST_CASE("127.0.0.0/8 is loopback")
{
    REQUIRE(IsIpv4Loopback(MakeIpv4(127, 0, 0, 1)));
    REQUIRE(IsIpv4Loopback(MakeIpv4(127, 255, 1, 2)));
    REQUIRE_FALSE(IsIpv4Loopback(MakeIpv4(10, 0, 2, 15)));
}
