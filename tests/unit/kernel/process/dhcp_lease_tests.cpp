#include <catch2/catch_test_macros.hpp>

#include "user/dhcp_lease.h"

namespace Dhcp = FeatherOS::User::Dhcp;

namespace
{

constexpr uint64_t Start = 5000;
constexpr uint64_t Day = 86400ULL * 1000;

} // namespace

TEST_CASE("a lease renews at half and rebinds at seven eighths unless the server says otherwise")
{
    const Dhcp::LeaseTimes slirp = Dhcp::ScheduleLease(Start, 86400, 0, 0);
    REQUIRE(slirp.renew_at == Start + Day / 2);
    REQUIRE(slirp.rebind_at == Start + Day / 8 * 7);
    REQUIRE(slirp.expire_at == Start + Day);

    const Dhcp::LeaseTimes given = Dhcp::ScheduleLease(Start, 3600, 600, 3000);
    REQUIRE(given.renew_at == Start + 600 * 1000);
    REQUIRE(given.rebind_at == Start + 3000 * 1000);
    REQUIRE(given.expire_at == Start + 3600 * 1000);
}

TEST_CASE("times out of order fall back to the defaults")
{
    const Dhcp::LeaseTimes late_rebind = Dhcp::ScheduleLease(Start, 3600, 600, 7200);
    REQUIRE(late_rebind.renew_at == Start + 600 * 1000);
    REQUIRE(late_rebind.rebind_at == Start + 3150 * 1000);

    const Dhcp::LeaseTimes late_renew = Dhcp::ScheduleLease(Start, 3600, 3400, 3000);
    REQUIRE(late_renew.renew_at == Start + 1800 * 1000);
    REQUIRE(late_renew.rebind_at == Start + 3000 * 1000);

    const Dhcp::LeaseTimes early_rebind = Dhcp::ScheduleLease(Start, 3600, 0, 1000);
    REQUIRE(early_rebind.renew_at == Start + 1000 * 1000);
    REQUIRE(early_rebind.rebind_at == Start + 1000 * 1000);
}

TEST_CASE("an infinite or missing lease never renews or ends")
{
    for (const uint32_t seconds : {Dhcp::InfiniteLease, 0U})
    {
        const Dhcp::LeaseTimes times = Dhcp::ScheduleLease(Start, seconds, 10, 20);
        REQUIRE(times.renew_at == Dhcp::Never);
        REQUIRE(times.rebind_at == Dhcp::Never);
        REQUIRE(times.expire_at == Dhcp::Never);
        REQUIRE(Dhcp::PhaseAt(times, UINT64_MAX - 1) == Dhcp::LeasePhase::Bound);
    }
}

TEST_CASE("the phase follows T1, T2 and the end of the lease")
{
    const Dhcp::LeaseTimes times = Dhcp::ScheduleLease(Start, 86400, 0, 0);
    REQUIRE(Dhcp::PhaseAt(times, Start) == Dhcp::LeasePhase::Bound);
    REQUIRE(Dhcp::PhaseAt(times, times.renew_at - 1) == Dhcp::LeasePhase::Bound);
    REQUIRE(Dhcp::PhaseAt(times, times.renew_at) == Dhcp::LeasePhase::Renewing);
    REQUIRE(Dhcp::PhaseAt(times, times.rebind_at) == Dhcp::LeasePhase::Rebinding);
    REQUIRE(Dhcp::PhaseAt(times, times.expire_at) == Dhcp::LeasePhase::Expired);
}

TEST_CASE("retries wait half the time left, at least a minute, and stop at T2 or the end")
{
    const Dhcp::LeaseTimes times = Dhcp::ScheduleLease(Start, 86400, 0, 0);
    // Renewing: 32400 s until T2.
    REQUIRE(Dhcp::RetryAt(times, times.renew_at) == times.renew_at + 16200 * 1000);
    REQUIRE(Dhcp::RetryAt(times, times.rebind_at - 200 * 1000) == times.rebind_at - 100 * 1000);
    REQUIRE(Dhcp::RetryAt(times, times.rebind_at - 100 * 1000) == times.rebind_at - 40 * 1000);
    REQUIRE(Dhcp::RetryAt(times, times.rebind_at - 1000) == times.rebind_at);
    // Rebinding: 10800 s until the end.
    REQUIRE(Dhcp::RetryAt(times, times.rebind_at) == times.rebind_at + 5400 * 1000);
    REQUIRE(Dhcp::RetryAt(times, times.expire_at - 30 * 1000) == times.expire_at);
    // A renewal asked for while bound counts towards T2.
    REQUIRE(Dhcp::RetryAt(times, Start) == Start + Day / 16 * 7);
}

TEST_CASE("discovery backs off from a second to a minute")
{
    REQUIRE(Dhcp::DiscoverTimeout(0) == 1000);
    REQUIRE(Dhcp::DiscoverTimeout(1) == 2000);
    REQUIRE(Dhcp::DiscoverTimeout(5) == 32000);
    REQUIRE(Dhcp::DiscoverTimeout(6) == 64000);
    REQUIRE(Dhcp::DiscoverTimeout(100) == 64000);
}
