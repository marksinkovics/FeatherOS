#include <catch2/catch_test_macros.hpp>

#include "kernel/arch/rtc.h"
#include "kernel/time/wall_clock.h"
#include "support/scheduler_stubs.h"

namespace
{

uint64_t rtc_seconds = 0;
int rtc_reads = 0;

uint64_t Read()
{
    uint64_t nanoseconds = 0;
    REQUIRE(FeatherOS::Time::ReadWallClock(&nanoseconds));
    return nanoseconds;
}

} // namespace

namespace FeatherOS::Arch::RealTimeClock
{

bool ReadUnixSeconds(uint64_t* seconds)
{
    ++rtc_reads;
    *seconds = rtc_seconds;
    return true;
}

} // namespace FeatherOS::Arch::RealTimeClock

// The clock keeps its base between test cases, so one case covers its whole life.
TEST_CASE("the wall clock counts ticks from one RTC reading, then from the time it was set")
{
    TestScheduler::Reset();
    TestScheduler::ticks = 500;
    rtc_seconds = 1792364283;
    REQUIRE(Read() == 1792364283000000000);
    REQUIRE(rtc_reads == 1);

    TestScheduler::ticks = 501;
    rtc_seconds = 1;
    REQUIRE(Read() == 1792364283010000000);
    TestScheduler::ticks = 500 + 100ULL * 86400 * 365 + 37;
    REQUIRE(Read() == 1792364283000000000 + 86400ULL * 365 * 1000000000 + 370000000);
    REQUIRE(rtc_reads == 1);

    REQUIRE(FeatherOS::Time::SetWallClock(1924992000123456789));
    REQUIRE(Read() == 1924992000123456789);
    TestScheduler::ticks += 250;
    REQUIRE(Read() == 1924992002623456789);
    REQUIRE(FeatherOS::Time::SetWallClock(5));
    REQUIRE(Read() == 5);
    REQUIRE(rtc_reads == 1);

    REQUIRE_FALSE(FeatherOS::Time::SetWallClock(UINT64_MAX - 1));
    REQUIRE(Read() == 5);
}
