#include <catch2/catch_test_macros.hpp>

#include "kernel/network/address.h"
#include "kernel/network/checksum.h"
#include "kernel/network/icmp.h"
#include "kernel/network/ipv4.h"
#include "kernel/network/udp.h"

#include <vector>

using namespace FeatherOS::Network;

namespace
{

struct EchoMessage
{
    EchoMessage()
    {
        data[0] = IcmpEchoRequest;
        WriteBig16(data + 6, 5);
        for (size_t index = IcmpHeaderSize; index < sizeof(data); ++index)
        {
            data[index] = static_cast<uint8_t>(index);
        }
        RewriteIcmpEcho(data, sizeof(data), IcmpEchoRequest, 0x4242);
    }

    uint8_t data[IcmpHeaderSize + 9] = {};
};

} // namespace

TEST_CASE("an echo request gets its identifier and a valid checksum")
{
    EchoMessage message;
    REQUIRE(InternetChecksum(message.data, sizeof(message.data)) == 0);
    IcmpEcho echo;
    REQUIRE(ParseIcmpEcho(message.data, sizeof(message.data), &echo));
    REQUIRE(echo.type == IcmpEchoRequest);
    REQUIRE(echo.identifier == 0x4242);
    REQUIRE(echo.sequence == 5);
}

TEST_CASE("an echo reply keeps the request's sequence number and data")
{
    EchoMessage message;
    RewriteIcmpEcho(message.data, sizeof(message.data), IcmpEchoReply, 0x4242);
    IcmpEcho echo;
    REQUIRE(ParseIcmpEcho(message.data, sizeof(message.data), &echo));
    REQUIRE(echo.type == IcmpEchoReply);
    REQUIRE(echo.identifier == 0x4242);
    REQUIRE(echo.sequence == 5);
    REQUIRE(message.data[sizeof(message.data) - 1] == sizeof(message.data) - 1);
}

TEST_CASE("other or damaged ICMP messages are not echoes")
{
    EchoMessage message;
    IcmpEcho echo;
    REQUIRE_FALSE(ParseIcmpEcho(message.data, IcmpHeaderSize - 1, &echo));
    SECTION("a damaged byte")
    {
        message.data[IcmpHeaderSize] ^= 1;
    }
    SECTION("a nonzero code")
    {
        message.data[1] = 1;
        WriteBig16(message.data + 2, 0);
        WriteBig16(message.data + 2, InternetChecksum(message.data, sizeof(message.data)));
    }
    SECTION("destination unreachable")
    {
        RewriteIcmpEcho(message.data, sizeof(message.data), 3, 0x4242);
    }
    REQUIRE_FALSE(ParseIcmpEcho(message.data, sizeof(message.data), &echo));
}

namespace
{

constexpr Ipv4Address Sender = MakeIpv4(10, 0, 2, 2);
constexpr Ipv4Address Receiver = MakeIpv4(10, 0, 2, 15);

std::vector<uint8_t> UdpPacket(size_t payload_length)
{
    std::vector<uint8_t> datagram(UdpHeaderSize + payload_length, 0x5A);
    WriteUdpHeader(datagram.data(), 4000, 9, payload_length);
    const Ipv4Packet fields = {
        .source = Sender,
        .destination = Receiver,
        .protocol = IpProtocolUdp,
        .payload = datagram.data(),
        .payload_length = datagram.size(),
    };
    uint8_t packet[MaximumIpv4PacketSize];
    const size_t length = BuildIpv4Packet(packet, fields, 7);
    return {packet, packet + length};
}

} // namespace

TEST_CASE("a port unreachable error quotes the datagram and parses back")
{
    const std::vector<uint8_t> packet = UdpPacket(12);
    uint8_t message[MaximumIcmpErrorSize];
    const size_t length = BuildIcmpPortUnreachable(message, packet.data(), packet.size());
    REQUIRE(length == IcmpHeaderSize + packet.size());
    REQUIRE(message[0] == IcmpDestinationUnreachable);
    REQUIRE(message[1] == IcmpPortUnreachable);
    REQUIRE(ReadBig32(message + 4) == 0);
    REQUIRE(InternetChecksum(message, length) == 0);
    REQUIRE(std::vector<uint8_t>(message + IcmpHeaderSize, message + length) == packet);

    IcmpUnreachable error{};
    REQUIRE(ParseIcmpUnreachable(message, length, &error));
    REQUIRE(error.code == IcmpPortUnreachable);
    REQUIRE(error.protocol == IpProtocolUdp);
    REQUIRE(error.source == MakeIpAddress(Sender));
    REQUIRE(error.destination == MakeIpAddress(Receiver));
    REQUIRE(error.source_port == 4000);
    REQUIRE(error.destination_port == 9);
}

TEST_CASE("a port unreachable error stays within 576 bytes of IPv4")
{
    const std::vector<uint8_t> packet = UdpPacket(1000);
    uint8_t message[MaximumIcmpErrorSize];
    const size_t length = BuildIcmpPortUnreachable(message, packet.data(), packet.size());
    REQUIRE(length == MaximumIcmpErrorSize);
    REQUIRE(Ipv4HeaderSize + length == 576);
    REQUIRE(InternetChecksum(message, length) == 0);
    IcmpUnreachable error{};
    REQUIRE(ParseIcmpUnreachable(message, length, &error));
    REQUIRE(error.destination_port == 9);
}

TEST_CASE("unreachable errors that are damaged or quote too little are refused")
{
    const std::vector<uint8_t> packet = UdpPacket(0);
    uint8_t message[MaximumIcmpErrorSize];
    const size_t length = BuildIcmpPortUnreachable(message, packet.data(), packet.size());
    IcmpUnreachable error{};
    REQUIRE_FALSE(ParseIcmpUnreachable(message, length - 1, &error));
    message[length - 1] ^= 1;
    REQUIRE_FALSE(ParseIcmpUnreachable(message, length, &error));
    message[length - 1] ^= 1;

    EchoMessage echo;
    REQUIRE_FALSE(ParseIcmpUnreachable(echo.data, sizeof(echo.data), &error));
}

TEST_CASE("ICMP errors come in bursts, then one per interval")
{
    IcmpErrorLimit limit = {.tokens = IcmpErrorBurst, .refilled_at_ms = 0};
    for (uint32_t index = 0; index < IcmpErrorBurst; ++index)
    {
        REQUIRE(TakeIcmpErrorToken(&limit, 1000));
    }
    REQUIRE_FALSE(TakeIcmpErrorToken(&limit, 1000));
    REQUIRE_FALSE(TakeIcmpErrorToken(&limit, 1000 + IcmpErrorIntervalMs - 1));
    REQUIRE(TakeIcmpErrorToken(&limit, 1000 + IcmpErrorIntervalMs));
    REQUIRE_FALSE(TakeIcmpErrorToken(&limit, 1000 + IcmpErrorIntervalMs));
    // A partial interval carries over to the next token.
    REQUIRE(TakeIcmpErrorToken(&limit, 1000 + 2 * IcmpErrorIntervalMs + 50));
    REQUIRE(TakeIcmpErrorToken(&limit, 1000 + 3 * IcmpErrorIntervalMs));

    // A long pause refills the bucket, but only to the burst.
    for (uint32_t index = 0; index < IcmpErrorBurst; ++index)
    {
        REQUIRE(TakeIcmpErrorToken(&limit, 100000));
    }
    REQUIRE_FALSE(TakeIcmpErrorToken(&limit, 100000));
}
