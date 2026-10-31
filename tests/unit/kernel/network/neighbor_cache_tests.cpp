#include <catch2/catch_test_macros.hpp>

#include "kernel/network/neighbor_cache.h"

using namespace FeatherOS::Network;

namespace
{

MacAddress HostMac(uint8_t last)
{
    return {{0x02, 0x00, 0x00, 0x00, 0x00, last}};
}

IpAddress Ipv6Host(uint8_t last)
{
    IpAddress address = {.version = IpVersion::V6, .bytes = {0xFE, 0x80}};
    address.bytes[15] = last;
    return address;
}

} // namespace

TEST_CASE("the neighbor cache answers until an entry expires")
{
    NeighborCache cache;
    const IpAddress gateway = MakeIpAddress(MakeIpv4(10, 0, 2, 2));
    MacAddress mac;
    REQUIRE_FALSE(cache.Lookup(gateway, 0, &mac));

    cache.Insert(gateway, HostMac(2), 100);
    REQUIRE(cache.Lookup(gateway, 99, &mac));
    REQUIRE(mac == HostMac(2));
    REQUIRE(cache.Contains(gateway, 99));
    REQUIRE_FALSE(cache.Lookup(gateway, 100, &mac));

    cache.Insert(gateway, HostMac(3), 200);
    REQUIRE(cache.Lookup(gateway, 150, &mac));
    REQUIRE(mac == HostMac(3));
}

TEST_CASE("IPv4 and IPv6 neighbors share the cache without mixing")
{
    NeighborCache cache;
    // The same leading bytes, as an IPv4 address and as an IPv6 one.
    IpAddress ipv4 = MakeIpAddress(MakeIpv4(254, 128, 0, 0));
    IpAddress ipv6 = ipv4;
    ipv6.version = IpVersion::V6;
    cache.Insert(ipv4, HostMac(4), 100);
    REQUIRE_FALSE(cache.Contains(ipv6, 0));
    cache.Insert(Ipv6Host(2), HostMac(6), 100);
    MacAddress mac;
    REQUIRE(cache.Lookup(Ipv6Host(2), 0, &mac));
    REQUIRE(mac == HostMac(6));
    REQUIRE(cache.Lookup(ipv4, 0, &mac));
    REQUIRE(mac == HostMac(4));
}

TEST_CASE("a full neighbor cache replaces the entry closest to expiring")
{
    NeighborCache cache;
    for (uint8_t index = 0; index < NeighborCache::Capacity; ++index)
    {
        cache.Insert(Ipv6Host(100 + index), HostMac(index), 1000 + index);
    }
    // Refreshing a known address keeps every other entry.
    cache.Insert(Ipv6Host(105), HostMac(55), 5000);
    for (uint8_t index = 0; index < NeighborCache::Capacity; ++index)
    {
        REQUIRE(cache.Contains(Ipv6Host(100 + index), 0));
    }

    cache.Insert(Ipv6Host(2), HostMac(2), 2000);
    MacAddress mac;
    REQUIRE(cache.Lookup(Ipv6Host(2), 0, &mac));
    REQUIRE(mac == HostMac(2));
    REQUIRE_FALSE(cache.Contains(Ipv6Host(100), 0));
    REQUIRE(cache.Lookup(Ipv6Host(105), 0, &mac));
    REQUIRE(mac == HostMac(55));
}

TEST_CASE("clearing the neighbor cache forgets every entry")
{
    NeighborCache cache;
    const IpAddress gateway = MakeIpAddress(MakeIpv4(10, 0, 2, 2));
    cache.Insert(gateway, HostMac(2), 100);
    cache.Insert(Ipv6Host(2), HostMac(3), 100);
    cache.Clear();
    MacAddress mac;
    REQUIRE_FALSE(cache.Lookup(gateway, 0, &mac));
    REQUIRE_FALSE(cache.Lookup(Ipv6Host(2), 0, &mac));
    cache.Insert(gateway, HostMac(4), 100);
    REQUIRE(cache.Lookup(gateway, 0, &mac));
    REQUIRE(mac == HostMac(4));
}
