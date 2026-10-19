#include <catch2/catch_test_macros.hpp>

#include "user/calendar.h"

#include <string>

namespace
{

using FeatherOS::User::CalendarTime;

std::string Format(uint64_t seconds)
{
    char buffer[FeatherOS::User::FormattedCalendarTimeLength];
    REQUIRE(FeatherOS::User::FormatCalendarTime(
        FeatherOS::User::CalendarFromUnixSeconds(seconds), buffer));
    return buffer;
}

uint64_t Seconds(uint32_t year, uint32_t month, uint32_t day, uint32_t hour, uint32_t minute,
                 uint32_t second)
{
    uint64_t seconds = 0;
    REQUIRE(FeatherOS::User::UnixSecondsFromCalendar({year, month, day, hour, minute, second},
                                                     &seconds));
    return seconds;
}

} // namespace

TEST_CASE("calendar converts known UNIX timestamps to UTC dates")
{
    REQUIRE(Format(0) == "1970-01-01 00:00:00 UTC");
    REQUIRE(Format(951782400) == "2000-02-29 00:00:00 UTC");
    REQUIRE(Format(1735689599) == "2024-12-31 23:59:59 UTC");
    REQUIRE(Format(4107542400) == "2100-03-01 00:00:00 UTC");
}

TEST_CASE("calendar converts dates back to UNIX timestamps")
{
    REQUIRE(Seconds(1970, 1, 1, 0, 0, 0) == 0);
    REQUIRE(Seconds(2000, 2, 29, 0, 0, 0) == 951782400);
    REQUIRE(Seconds(2024, 12, 31, 23, 59, 59) == 1735689599);
    REQUIRE(Seconds(2026, 10, 18, 22, 58, 3) == 1792364283);
}

TEST_CASE("calendar round-trips every day across leap and century years")
{
    for (uint64_t day = 0; day < 365 * 450; day += 1)
    {
        const uint64_t seconds = day * 86400 + 12345;
        const CalendarTime time = FeatherOS::User::CalendarFromUnixSeconds(seconds);
        uint64_t round_trip = 0;
        REQUIRE(FeatherOS::User::UnixSecondsFromCalendar(time, &round_trip));
        REQUIRE(round_trip == seconds);
    }
}

TEST_CASE("calendar rejects invalid dates")
{
    uint64_t seconds = 0;
    REQUIRE_FALSE(FeatherOS::User::UnixSecondsFromCalendar({1969, 12, 31, 0, 0, 0}, &seconds));
    REQUIRE_FALSE(FeatherOS::User::UnixSecondsFromCalendar({2023, 2, 29, 0, 0, 0}, &seconds));
    REQUIRE_FALSE(FeatherOS::User::UnixSecondsFromCalendar({2100, 2, 29, 0, 0, 0}, &seconds));
    REQUIRE_FALSE(FeatherOS::User::UnixSecondsFromCalendar({2024, 13, 1, 0, 0, 0}, &seconds));
    REQUIRE_FALSE(FeatherOS::User::UnixSecondsFromCalendar({2024, 4, 31, 0, 0, 0}, &seconds));
    REQUIRE_FALSE(FeatherOS::User::UnixSecondsFromCalendar({2024, 1, 1, 24, 0, 0}, &seconds));
    REQUIRE_FALSE(FeatherOS::User::UnixSecondsFromCalendar({2024, 1, 1, 0, 60, 0}, &seconds));
    REQUIRE_FALSE(FeatherOS::User::UnixSecondsFromCalendar({2024, 1, 1, 0, 0, 60}, &seconds));
}
