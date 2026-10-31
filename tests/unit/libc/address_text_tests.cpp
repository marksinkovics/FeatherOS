#include <catch2/catch_test_macros.hpp>

#include "libc/address_text.h"

#include <cstdio>
#include <string>

using FeatherOS::LibC::FormatIpv4Text;
using FeatherOS::LibC::FormatIpv6Text;
using FeatherOS::LibC::Ipv4TextCapacity;
using FeatherOS::LibC::Ipv6TextCapacity;
using FeatherOS::LibC::ParseAddressText;
using FeatherOS::LibC::ParseIpv4Text;
using FeatherOS::LibC::ParseIpv6Text;

namespace
{

std::string Groups(const uint8_t (&address)[16])
{
    std::string text;
    for (int index = 0; index < 16; index += 2)
    {
        char group[8];
        snprintf(group, sizeof(group), "%s%x", index == 0 ? "" : ":",
                 (address[index] << 8) | address[index + 1]);
        text += group;
    }
    return text;
}

std::string Format(std::initializer_list<uint16_t> groups)
{
    uint8_t address[16] = {};
    int index = 0;
    for (uint16_t group : groups)
    {
        address[index++] = static_cast<uint8_t>(group >> 8);
        address[index++] = static_cast<uint8_t>(group);
    }
    char text[Ipv6TextCapacity];
    REQUIRE(FormatIpv6Text(address, text, sizeof(text)) > 0);
    return text;
}

} // namespace

TEST_CASE("dotted IPv4 addresses parse in network order")
{
    uint8_t address[4] = {};
    REQUIRE(ParseIpv4Text("10.0.2.15", address));
    REQUIRE(address[0] == 10);
    REQUIRE(address[1] == 0);
    REQUIRE(address[2] == 2);
    REQUIRE(address[3] == 15);
    REQUIRE(ParseIpv4Text("255.255.255.255", address));
    REQUIRE(address[3] == 255);
    REQUIRE(ParseIpv4Text("0.0.0.0", address));
    REQUIRE(address[0] == 0);
}

TEST_CASE("anything but four plain decimal parts is refused")
{
    uint8_t address[4] = {1, 2, 3, 4};
    for (const char* text : {"", "10.0.2", "10.0.2.15.1", "10.0.2.256", "10.0.2.015", "10..2.15",
                             "10.0.2.15 ", " 10.0.2.15", "10.0.2.x", "1000.0.0.1", "0x0A.0.2.15",
                             "nowhere"})
    {
        INFO(text);
        REQUIRE_FALSE(ParseIpv4Text(text, address));
    }
    REQUIRE_FALSE(ParseIpv4Text(nullptr, address));
    REQUIRE(address[0] == 1);
}

TEST_CASE("IPv4 addresses format as dotted decimal")
{
    char text[Ipv4TextCapacity];
    const uint8_t address[4] = {192, 168, 0, 199};
    REQUIRE(FormatIpv4Text(address, text, sizeof(text)) == 13);
    REQUIRE(std::string(text) == "192.168.0.199");

    const uint8_t longest[4] = {255, 255, 255, 255};
    REQUIRE(FormatIpv4Text(longest, text, sizeof(text)) == 15);
    REQUIRE(FormatIpv4Text(longest, text, 15) == 0);
    REQUIRE(FormatIpv4Text(address, nullptr, sizeof(text)) == 0);
}

TEST_CASE("IPv6 addresses parse with and without compression")
{
    uint8_t address[16] = {};
    REQUIRE(ParseIpv6Text("::", address));
    REQUIRE(Groups(address) == "0:0:0:0:0:0:0:0");
    REQUIRE(ParseIpv6Text("::1", address));
    REQUIRE(Groups(address) == "0:0:0:0:0:0:0:1");
    REQUIRE(ParseIpv6Text("fe80::5054:ff:fe12:3456", address));
    REQUIRE(Groups(address) == "fe80:0:0:0:5054:ff:fe12:3456");
    REQUIRE(ParseIpv6Text("FEC0::2", address));
    REQUIRE(Groups(address) == "fec0:0:0:0:0:0:0:2");
    REQUIRE(ParseIpv6Text("1:2:3:4:5:6:7:8", address));
    REQUIRE(Groups(address) == "1:2:3:4:5:6:7:8");
    REQUIRE(ParseIpv6Text("1::", address));
    REQUIRE(Groups(address) == "1:0:0:0:0:0:0:0");
    REQUIRE(ParseIpv6Text("1:2:3:4:5:6:7::", address));
    REQUIRE(Groups(address) == "1:2:3:4:5:6:7:0");
    REQUIRE(ParseIpv6Text("::ffff:10.0.2.15", address));
    REQUIRE(Groups(address) == "0:0:0:0:0:ffff:a00:20f");
    REQUIRE(ParseIpv6Text("1:2:3:4:5:6:10.0.2.15", address));
    REQUIRE(Groups(address) == "1:2:3:4:5:6:a00:20f");
}

TEST_CASE("malformed IPv6 text is refused")
{
    uint8_t address[16] = {1};
    for (const char* text :
         {"", ":", ":::", "1::2::3", "12345::", "1:2:3:4:5:6:7:8:9", "1:2:3:4:5:6:7", ":1::",
          "1:", "1::2:", "g::", "::ffff:1.2.3", "::1.2.3.4:5", "1:2:3:4:5:6:7:1.2.3.4",
          "fe80::1%eth0", " ::1", "10.0.2.15"})
    {
        INFO(text);
        REQUIRE_FALSE(ParseIpv6Text(text, address));
    }
    REQUIRE_FALSE(ParseIpv6Text(nullptr, address));
    REQUIRE(address[0] == 1);
}

TEST_CASE("IPv6 addresses format as RFC 5952 recommends")
{
    REQUIRE(Format({}) == "::");
    REQUIRE(Format({0, 0, 0, 0, 0, 0, 0, 1}) == "::1");
    REQUIRE(Format({0xFE80, 0, 0, 0, 0x5054, 0xFF, 0xFE12, 0x3456}) ==
            "fe80::5054:ff:fe12:3456");
    REQUIRE(Format({1, 0, 0, 0, 0, 0, 0, 0}) == "1::");
    // A single zero group stays, and the longer run, or the first of two equal runs, is
    // compressed.
    REQUIRE(Format({0x2001, 0xDB8, 0, 1, 1, 1, 1, 1}) == "2001:db8:0:1:1:1:1:1");
    REQUIRE(Format({1, 0, 0, 2, 0, 0, 0, 3}) == "1:0:0:2::3");
    REQUIRE(Format({1, 0, 0, 4, 0, 0, 7, 8}) == "1::4:0:0:7:8");
    REQUIRE(Format({0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}) ==
            "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff");

    const uint8_t loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    char text[Ipv6TextCapacity];
    REQUIRE(FormatIpv6Text(loopback, text, 4) == 3);
    REQUIRE(FormatIpv6Text(loopback, text, 3) == 0);
}

TEST_CASE("addresses of either version parse from text without a terminator")
{
    const std::string text = "10.0.2.15 fec0::2 nonsense";
    uint8_t address[16] = {};
    REQUIRE(ParseAddressText(text.data(), 9, address) == 4);
    REQUIRE(address[0] == 10);
    REQUIRE(address[3] == 15);
    REQUIRE(ParseAddressText(text.data() + 10, 7, address) == 16);
    REQUIRE(address[0] == 0xFE);
    REQUIRE(address[15] == 2);
    REQUIRE(ParseAddressText(text.data(), 10, address) == 0);
    REQUIRE(ParseAddressText(text.data() + 18, 8, address) == 0);
    REQUIRE(ParseAddressText(text.data(), 0, address) == 0);
    REQUIRE(ParseAddressText(std::string(Ipv6TextCapacity, '1').c_str(), Ipv6TextCapacity,
                             address) == 0);
}
