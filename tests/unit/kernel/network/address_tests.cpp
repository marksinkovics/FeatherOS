#include <catch2/catch_test_macros.hpp>

#include "kernel/network/address.h"

#include <string>

using namespace FeatherOS::Network;

TEST_CASE("addresses format as text")
{
    char mac[FormattedMacLength];
    FormatMac({{0x52, 0x54, 0x00, 0x12, 0x34, 0xAB}}, mac);
    REQUIRE(std::string(mac) == "52:54:00:12:34:ab");

    char ipv4[FormattedIpv4Length];
    FormatIpv4(MakeIpv4(10, 0, 2, 15), ipv4);
    REQUIRE(std::string(ipv4) == "10.0.2.15");
    FormatIpv4(MakeIpv4(255, 255, 255, 255), ipv4);
    REQUIRE(std::string(ipv4) == "255.255.255.255");
    FormatIpv4(0, ipv4);
    REQUIRE(std::string(ipv4) == "0.0.0.0");
}

TEST_CASE("netmasks give their prefix length")
{
    REQUIRE(PrefixLength(MakeIpv4(255, 255, 255, 0)) == 24);
    REQUIRE(PrefixLength(MakeIpv4(255, 255, 240, 0)) == 20);
    REQUIRE(PrefixLength(0xFFFFFFFFU) == 32);
    REQUIRE(PrefixLength(0) == 0);
    REQUIRE(PrefixNetmask(24) == MakeIpv4(255, 255, 255, 0));
    REQUIRE(PrefixNetmask(20) == MakeIpv4(255, 255, 240, 0));
    REQUIRE(PrefixNetmask(32) == 0xFFFFFFFFU);
    REQUIRE(PrefixNetmask(0) == 0);
}

TEST_CASE("multi-byte fields are big-endian on the wire")
{
    uint8_t bytes[4] = {};
    WriteBig16(bytes, 0x0806);
    REQUIRE(bytes[0] == 0x08);
    REQUIRE(bytes[1] == 0x06);
    REQUIRE(ReadBig16(bytes) == 0x0806);
    WriteBig32(bytes, MakeIpv4(10, 0, 2, 2));
    REQUIRE(bytes[0] == 10);
    REQUIRE(bytes[3] == 2);
    REQUIRE(ReadBig32(bytes) == MakeIpv4(10, 0, 2, 2));
}

TEST_CASE("an IPv4 address fits the family-neutral address")
{
    const IpAddress address = MakeIpAddress(MakeIpv4(10, 0, 2, 15));
    REQUIRE(address.version == IpVersion::V4);
    REQUIRE(address.bytes[0] == 10);
    REQUIRE(address.bytes[3] == 15);
    REQUIRE(address.bytes[4] == 0);
    REQUIRE(ToIpv4(address) == MakeIpv4(10, 0, 2, 15));
    REQUIRE(address == MakeIpAddress(MakeIpv4(10, 0, 2, 15)));
    REQUIRE_FALSE(address == MakeIpAddress(MakeIpv4(10, 0, 2, 2)));
}

TEST_CASE("addresses of different versions differ even with the same bytes")
{
    IpAddress ipv6 = MakeIpAddress(0);
    ipv6.version = IpVersion::V6;
    REQUIRE_FALSE(ipv6 == MakeIpAddress(0));
    REQUIRE(ToIpv4(ipv6) == 0);
}

TEST_CASE("the all-zero address is unspecified")
{
    REQUIRE(IsUnspecified(MakeIpAddress(0)));
    REQUIRE_FALSE(IsUnspecified(MakeIpAddress(MakeIpv4(0, 0, 0, 1))));
    REQUIRE_FALSE(IsUnspecified(IpAddress{}));
}

TEST_CASE("IPv6 addresses format with the longest zero run compressed")
{
    const auto format = [](std::initializer_list<uint16_t> groups) {
        IpAddress address = {.version = IpVersion::V6, .bytes = {}};
        size_t index = 0;
        for (uint16_t group : groups)
        {
            WriteBig16(address.bytes + index, group);
            index += 2;
        }
        char text[FormattedIpv6Length];
        FormatIpv6(address, text);
        return std::string(text);
    };
    REQUIRE(format({}) == "::");
    REQUIRE(format({0, 0, 0, 0, 0, 0, 0, 1}) == "::1");
    REQUIRE(format({1, 0, 0, 0, 0, 0, 0, 0}) == "1::");
    REQUIRE(format({0xFE80, 0, 0, 0, 0x5054, 0xFF, 0xFE12, 0x3456}) == "fe80::5054:ff:fe12:3456");
    REQUIRE(format({0x2001, 0xDB8, 0, 1, 1, 1, 1, 1}) == "2001:db8:0:1:1:1:1:1");
    REQUIRE(format({1, 0, 0, 2, 0, 0, 0, 3}) == "1:0:0:2::3");
    REQUIRE(format({1, 0, 0, 4, 0, 0, 7, 8}) == "1::4:0:0:7:8");
    REQUIRE(format({0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}) ==
            "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff");
}
