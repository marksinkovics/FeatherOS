#include <catch2/catch_test_macros.hpp>

#include "kernel/network/checksum.h"
#include "kernel/network/icmpv6.h"
#include "kernel/network/ipv6.h"
#include "kernel/network/udp.h"

#include <vector>

using namespace FeatherOS::Network;

namespace
{

constexpr MacAddress Local = {{0x52, 0x54, 0x00, 0x12, 0x34, 0x56}};
constexpr MacAddress RouterMac = {{0x52, 0x55, 0x0A, 0x00, 0x02, 0x02}};

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

// A router advertisement as slirp sends it: hop limit 0, no flags, a 1800 s lifetime, then the
// options passed in.
std::vector<uint8_t> Advertisement(std::vector<uint8_t> options)
{
    std::vector<uint8_t> message = {RouterAdvertisementType, 0, 0, 0, 0, 0, 0x07, 0x08,
                                    0, 0, 0, 0, 0, 0, 0, 0};
    message.insert(message.end(), options.begin(), options.end());
    return message;
}

std::vector<uint8_t> PrefixOption(uint8_t flags, uint8_t first)
{
    std::vector<uint8_t> option(32, 0);
    option[0] = 3;
    option[1] = 4;
    option[2] = 64;
    option[3] = flags;
    option[7] = 100;
    option[16] = first;
    option[17] = 0xC0;
    return option;
}

} // namespace

TEST_CASE("the pseudo-header checksum matches an independent computation")
{
    uint8_t message[MaximumNeighborMessageSize];
    const size_t length = BuildRouterSolicitation(message, &Local);
    REQUIRE(length == 16);
    const uint64_t sum = PseudoHeaderSum(LinkLocal, AllRouters, IpProtocolIcmpv6, length);
    REQUIRE(ChecksumFinish(ChecksumAdd(sum, message, length)) == 0x71B5);
    WriteBig16(message + 2, 0x71B5);
    REQUIRE(ChecksumFinish(ChecksumAdd(sum, message, length)) == 0);
}

TEST_CASE("an IPv4 pseudo-header sums the addresses, protocol and length")
{
    const uint64_t sum = PseudoHeaderSum(MakeIpAddress(MakeIpv4(10, 0, 2, 15)),
                                         MakeIpAddress(MakeIpv4(10, 0, 2, 2)), 17, 12);
    REQUIRE(sum == 0x0A00 + 0x020F + 0x0A00 + 0x0202 + 17 + 12);
}

TEST_CASE("a neighbor solicitation round-trips with and without our link address")
{
    uint8_t message[MaximumNeighborMessageSize];
    NeighborMessage parsed;
    REQUIRE(BuildNeighborSolicitation(message, LinkLocal, &Local) == 32);
    REQUIRE(ParseNeighborMessage(message, 32, &parsed));
    REQUIRE(parsed.type == NeighborSolicitation);
    REQUIRE(parsed.target == LinkLocal);
    REQUIRE(parsed.has_link_address);
    REQUIRE(parsed.link_address == Local);

    REQUIRE(BuildNeighborSolicitation(message, LinkLocal, nullptr) == 24);
    REQUIRE(ParseNeighborMessage(message, 24, &parsed));
    REQUIRE_FALSE(parsed.has_link_address);
}

TEST_CASE("a neighbor advertisement carries its flags and target link address")
{
    uint8_t message[MaximumNeighborMessageSize];
    REQUIRE(BuildNeighborAdvertisement(message, LinkLocal, Local, true) == 32);
    NeighborMessage parsed;
    REQUIRE(ParseNeighborMessage(message, 32, &parsed));
    REQUIRE(parsed.type == NeighborAdvertisement);
    REQUIRE(parsed.solicited);
    REQUIRE(parsed.override_entry);
    REQUIRE_FALSE(parsed.router);
    REQUIRE(parsed.target == LinkLocal);
    REQUIRE(parsed.has_link_address);
    REQUIRE(parsed.link_address == Local);

    REQUIRE(BuildNeighborAdvertisement(message, LinkLocal, Local, false) == 32);
    REQUIRE(ParseNeighborMessage(message, 32, &parsed));
    REQUIRE_FALSE(parsed.solicited);
}

TEST_CASE("neighbor messages with bad options or lengths are rejected")
{
    uint8_t message[MaximumNeighborMessageSize];
    BuildNeighborSolicitation(message, LinkLocal, &Local);
    NeighborMessage parsed;
    REQUIRE_FALSE(ParseNeighborMessage(message, 23, &parsed));
    REQUIRE_FALSE(ParseNeighborMessage(message, 30, &parsed));
    message[25] = 0;
    REQUIRE_FALSE(ParseNeighborMessage(message, 32, &parsed));
    message[25] = 2;
    REQUIRE_FALSE(ParseNeighborMessage(message, 32, &parsed));
    message[25] = 1;
    message[1] = 1;
    REQUIRE_FALSE(ParseNeighborMessage(message, 32, &parsed));
}

TEST_CASE("a router advertisement gives the router's link address and an autonomous prefix")
{
    std::vector<uint8_t> options = {1, 1, 0x52, 0x55, 0x0A, 0x00, 0x02, 0x02};
    // An on-link-only prefix comes first and is passed over.
    const std::vector<uint8_t> on_link = PrefixOption(0x80, 0xFD);
    const std::vector<uint8_t> autonomous = PrefixOption(0xC0, 0xFE);
    options.insert(options.end(), on_link.begin(), on_link.end());
    options.insert(options.end(), autonomous.begin(), autonomous.end());
    const std::vector<uint8_t> message = Advertisement(options);

    RouterAdvertisement parsed;
    REQUIRE(ParseRouterAdvertisement(message.data(), message.size(), &parsed));
    REQUIRE(parsed.router_lifetime == 0x0708);
    REQUIRE(parsed.has_link_address);
    REQUIRE(parsed.link_address == RouterMac);
    REQUIRE(parsed.has_prefix);
    REQUIRE(parsed.prefix_length == 64);
    REQUIRE(parsed.valid_lifetime == 100);
    REQUIRE(parsed.prefix == Ipv6({0xFEC0, 0, 0, 0, 0, 0, 0, 0}));
    REQUIRE_FALSE(parsed.has_name_server);
}

TEST_CASE("a router advertisement may name a DNS server")
{
    // RFC 8106: reserved, lifetime, then one or more addresses; only the first is kept.
    std::vector<uint8_t> option = {25, 5, 0, 0, 0, 0, 0x04, 0xB0};
    const std::vector<uint8_t> first = {0xFE, 0xC0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3};
    option.insert(option.end(), first.begin(), first.end());
    option.insert(option.end(), 16, 0xAA);
    const std::vector<uint8_t> message = Advertisement(option);
    RouterAdvertisement parsed;
    REQUIRE(ParseRouterAdvertisement(message.data(), message.size(), &parsed));
    REQUIRE(parsed.has_name_server);
    REQUIRE(parsed.name_server == Ipv6({0xFEC0, 0, 0, 0, 0, 0, 0, 3}));
    REQUIRE(parsed.name_server_lifetime == 1200);

    // Without room for an address, the option is skipped.
    const std::vector<uint8_t> empty = Advertisement({25, 1, 0, 0, 0, 0, 0x04, 0xB0});
    REQUIRE(ParseRouterAdvertisement(empty.data(), empty.size(), &parsed));
    REQUIRE_FALSE(parsed.has_name_server);
}

TEST_CASE("router advertisements without options or with broken ones")
{
    RouterAdvertisement parsed;
    const std::vector<uint8_t> bare = Advertisement({});
    REQUIRE(ParseRouterAdvertisement(bare.data(), bare.size(), &parsed));
    REQUIRE_FALSE(parsed.has_prefix);
    REQUIRE_FALSE(parsed.has_link_address);
    REQUIRE_FALSE(ParseRouterAdvertisement(bare.data(), bare.size() - 1, &parsed));

    const std::vector<uint8_t> zero_length = Advertisement({1, 0, 0, 0, 0, 0, 0, 0});
    REQUIRE_FALSE(ParseRouterAdvertisement(zero_length.data(), zero_length.size(), &parsed));
    const std::vector<uint8_t> overlong = Advertisement({1, 2, 0, 0, 0, 0, 0, 0});
    REQUIRE_FALSE(ParseRouterAdvertisement(overlong.data(), overlong.size(), &parsed));
}

TEST_CASE("ICMPv6 echoes parse their identifier and sequence")
{
    const uint8_t request[] = {Icmpv6EchoRequest, 0, 0, 0, 0x12, 0x34, 0, 7, 'x'};
    IcmpEcho echo;
    REQUIRE(ParseIcmpv6Echo(request, sizeof(request), &echo));
    REQUIRE(echo.type == Icmpv6EchoRequest);
    REQUIRE(echo.identifier == 0x1234);
    REQUIRE(echo.sequence == 7);
    REQUIRE_FALSE(ParseIcmpv6Echo(request, 7, &echo));
    const uint8_t neighbor[] = {NeighborSolicitation, 0, 0, 0, 0, 0, 0, 0};
    REQUIRE_FALSE(ParseIcmpv6Echo(neighbor, sizeof(neighbor), &echo));
}

namespace
{

std::vector<uint8_t> Ipv6UdpPacket(size_t payload_length)
{
    std::vector<uint8_t> datagram(UdpHeaderSize + payload_length, 0x5A);
    WriteUdpHeader(datagram.data(), 4000, 9, payload_length);
    Ipv6Packet fields;
    fields.source = LinkLocal;
    fields.destination = Ipv6Loopback;
    fields.next_header = IpProtocolUdp;
    fields.hop_limit = 64;
    fields.payload = datagram.data();
    fields.payload_length = datagram.size();
    uint8_t packet[MaximumIpv6PacketSize];
    const size_t length = BuildIpv6Packet(packet, fields);
    return {packet, packet + length};
}

} // namespace

TEST_CASE("an ICMPv6 port unreachable error quotes the datagram and parses back")
{
    const std::vector<uint8_t> packet = Ipv6UdpPacket(12);
    uint8_t message[MaximumIcmpv6ErrorSize];
    const size_t length = BuildIcmpv6PortUnreachable(message, packet.data(), packet.size());
    REQUIRE(length == IcmpHeaderSize + packet.size());
    REQUIRE(message[0] == Icmpv6DestinationUnreachable);
    REQUIRE(message[1] == Icmpv6PortUnreachable);
    REQUIRE(ReadBig16(message + Icmpv6ChecksumOffset) == 0);
    REQUIRE(ReadBig32(message + 4) == 0);
    REQUIRE(std::vector<uint8_t>(message + IcmpHeaderSize, message + length) == packet);

    IcmpUnreachable error{};
    REQUIRE(ParseIcmpv6Unreachable(message, length, &error));
    REQUIRE(error.code == Icmpv6PortUnreachable);
    REQUIRE(error.protocol == IpProtocolUdp);
    REQUIRE(error.source == LinkLocal);
    REQUIRE(error.destination == Ipv6Loopback);
    REQUIRE(error.source_port == 4000);
    REQUIRE(error.destination_port == 9);
    REQUIRE_FALSE(ParseIcmpv6Unreachable(message, IcmpHeaderSize + Ipv6HeaderSize + 7, &error));
}

TEST_CASE("an ICMPv6 error stays within the minimum IPv6 MTU")
{
    const std::vector<uint8_t> packet = Ipv6UdpPacket(1400);
    uint8_t message[MaximumIcmpv6ErrorSize];
    const size_t length = BuildIcmpv6PortUnreachable(message, packet.data(), packet.size());
    REQUIRE(Ipv6HeaderSize + length == 1280);
    IcmpUnreachable error{};
    REQUIRE(ParseIcmpv6Unreachable(message, length, &error));

    message[IcmpHeaderSize] = 0x45;
    REQUIRE_FALSE(ParseIcmpv6Unreachable(message, length, &error));
}
