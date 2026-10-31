#include <catch2/catch_test_macros.hpp>

#include "kernel/network/ipv6.h"

using namespace FeatherOS::Network;

namespace
{

constexpr MacAddress Local = {{0x52, 0x54, 0x00, 0x12, 0x34, 0x56}};

IpAddress Ipv6(std::initializer_list<uint16_t> groups)
{
    IpAddress address = Ipv6Unspecified;
    size_t index = 0;
    for (uint16_t group : groups)
    {
        WriteBig16(address.bytes + index, group);
        index += 2;
    }
    return address;
}

const IpAddress LinkLocal = Ipv6({0xFE80, 0, 0, 0, 0x5054, 0x00FF, 0xFE12, 0x3456});
const IpAddress Router = Ipv6({0xFE80, 0, 0, 0, 0, 0, 0, 2});

size_t BuildSample(uint8_t (&packet)[MaximumIpv6PacketSize])
{
    const uint8_t payload[] = {'p', 'i', 'n', 'g'};
    return BuildIpv6Packet(packet, {.source = LinkLocal,
                                    .destination = Router,
                                    .next_header = IpProtocolIcmpv6,
                                    .hop_limit = 64,
                                    .payload = payload,
                                    .payload_length = sizeof(payload)});
}

} // namespace

TEST_CASE("addresses derive from the MAC as modified EUI-64")
{
    REQUIRE(LinkLocalAddress(Local) == LinkLocal);
    REQUIRE(PrefixAddress(Ipv6({0xFEC0, 0, 0, 0, 0xFFFF, 1, 2, 3}), Local) ==
            Ipv6({0xFEC0, 0, 0, 0, 0x5054, 0x00FF, 0xFE12, 0x3456}));
}

TEST_CASE("solicitations go to the solicited-node group and its multicast MAC")
{
    const IpAddress group = SolicitedNodeAddress(LinkLocal);
    REQUIRE(group == Ipv6({0xFF02, 0, 0, 0, 0, 1, 0xFF12, 0x3456}));
    REQUIRE(MulticastMac(group) == MacAddress{{0x33, 0x33, 0xFF, 0x12, 0x34, 0x56}});
    REQUIRE(MulticastMac(AllNodes) == MacAddress{{0x33, 0x33, 0, 0, 0, 1}});
}

TEST_CASE("IPv6 address classes")
{
    REQUIRE(IsLinkLocal(LinkLocal));
    REQUIRE(IsLinkLocal(Ipv6({0xFEBF, 0, 0, 0, 0, 0, 0, 1})));
    REQUIRE_FALSE(IsLinkLocal(Ipv6({0xFEC0, 0, 0, 0, 0, 0, 0, 1})));
    REQUIRE(IsMulticast(AllRouters));
    REQUIRE_FALSE(IsMulticast(LinkLocal));
    REQUIRE_FALSE(IsLinkLocal(MakeIpAddress(MakeIpv4(254, 128, 0, 1))));
    REQUIRE(SamePrefix64(LinkLocal, Router));
    REQUIRE_FALSE(SamePrefix64(LinkLocal, Ipv6({0xFEC0, 0, 0, 0, 0, 0, 0, 2})));
}

TEST_CASE("an IPv6 packet round-trips through its wire format")
{
    uint8_t packet[MaximumIpv6PacketSize];
    const size_t length = BuildSample(packet);
    REQUIRE(length == Ipv6HeaderSize + 4);
    REQUIRE(packet[0] == 0x60);
    REQUIRE(ReadBig16(packet + 4) == 4);

    Ipv6Packet parsed;
    REQUIRE(ParseIpv6Packet(packet, length + 10, &parsed));
    REQUIRE(parsed.source == LinkLocal);
    REQUIRE(parsed.destination == Router);
    REQUIRE(parsed.next_header == IpProtocolIcmpv6);
    REQUIRE(parsed.hop_limit == 64);
    REQUIRE(parsed.payload == packet + Ipv6HeaderSize);
    REQUIRE(parsed.payload_length == 4);
}

TEST_CASE("extension headers are skipped and fragments dropped")
{
    uint8_t packet[MaximumIpv6PacketSize];
    BuildSample(packet);
    // Insert an 8-byte hop-by-hop header (padding only) before the payload.
    for (size_t index = 4; index-- > 0;)
    {
        packet[Ipv6HeaderSize + 8 + index] = packet[Ipv6HeaderSize + index];
    }
    for (size_t index = 0; index < 8; ++index)
    {
        packet[Ipv6HeaderSize + index] = 0;
    }
    packet[Ipv6HeaderSize] = IpProtocolIcmpv6;
    packet[6] = 0;
    WriteBig16(packet + 4, 12);
    Ipv6Packet parsed;
    REQUIRE(ParseIpv6Packet(packet, Ipv6HeaderSize + 12, &parsed));
    REQUIRE(parsed.next_header == IpProtocolIcmpv6);
    REQUIRE(parsed.payload == packet + Ipv6HeaderSize + 8);
    REQUIRE(parsed.payload_length == 4);

    SECTION("a fragment header")
    {
        packet[Ipv6HeaderSize] = 44;
    }
    SECTION("an extension header longer than the packet")
    {
        packet[Ipv6HeaderSize + 1] = 1;
    }
    REQUIRE_FALSE(ParseIpv6Packet(packet, Ipv6HeaderSize + 12, &parsed));
}

TEST_CASE("malformed IPv6 packets are rejected")
{
    uint8_t packet[MaximumIpv6PacketSize];
    const size_t length = BuildSample(packet);
    Ipv6Packet parsed;
    REQUIRE_FALSE(ParseIpv6Packet(packet, Ipv6HeaderSize - 1, &parsed));
    REQUIRE_FALSE(ParseIpv6Packet(packet, length - 1, &parsed));
    packet[0] = 0x40;
    REQUIRE_FALSE(ParseIpv6Packet(packet, length, &parsed));
}

TEST_CASE("an IPv6 payload that does not fit is refused")
{
    static uint8_t payload[MaximumIpv6Payload + 1];
    uint8_t packet[MaximumIpv6PacketSize];
    REQUIRE(BuildIpv6Packet(packet, {.source = LinkLocal,
                                     .destination = Router,
                                     .next_header = IpProtocolIcmpv6,
                                     .hop_limit = 64,
                                     .payload = payload,
                                     .payload_length = sizeof(payload)}) == 0);
}
