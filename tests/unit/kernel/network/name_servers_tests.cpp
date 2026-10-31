#include <catch2/catch_test_macros.hpp>

#include "kernel/network/name_servers.h"
#include "support/scheduler_stubs.h"

using namespace FeatherOS::Network;

namespace
{

const IpAddress Advertised = {.version = IpVersion::V6,
                              .bytes = {0xFE, 0xC0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3}};

struct NameServerState
{
    NameServerState()
    {
        TestScheduler::Reset();
        SetNameServers(nullptr, 0);
        AdvertiseNameServer({}, 0);
    }
};

} // namespace

TEST_CASE("configured name servers come before the advertised one")
{
    NameServerState state;
    AdvertiseNameServer(Advertised, 100);
    const NameServer configured[] = {{MakeIpAddress(MakeIpv4(10, 0, 2, 3)), DnsPort},
                                     {MakeIpAddress(MakeIpv4(10, 0, 2, 2)), 5353}};
    SetNameServers(configured, 2);
    NameServer servers[MaximumNameServers];
    REQUIRE(ReadNameServers(servers) == 3);
    REQUIRE(servers[0].address == configured[0].address);
    REQUIRE(servers[1].port == 5353);
    REQUIRE(servers[2].address == Advertised);
    REQUIRE(servers[2].port == DnsPort);

    // Once its lifetime passes, the advertised server is gone.
    TestScheduler::ticks = 100;
    REQUIRE(ReadNameServers(servers) == 2);
}

TEST_CASE("the advertised server is not listed twice, nor past the limit")
{
    NameServerState state;
    AdvertiseNameServer(Advertised, 100);
    const NameServer same[] = {{Advertised, DnsPort}};
    SetNameServers(same, 1);
    NameServer servers[MaximumNameServers];
    REQUIRE(ReadNameServers(servers) == 1);

    const NameServer full[] = {{MakeIpAddress(1), DnsPort},
                               {MakeIpAddress(2), DnsPort},
                               {MakeIpAddress(3), DnsPort},
                               {MakeIpAddress(4), DnsPort}};
    SetNameServers(full, 4);
    REQUIRE(ReadNameServers(servers) == MaximumNameServers);
    REQUIRE(servers[2].address == MakeIpAddress(3));
}
