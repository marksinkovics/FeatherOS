#include <catch2/catch_test_macros.hpp>

#include "libc/time_format.h"

#include <ctime>
#include <string>

namespace
{

std::tm Sample()
{
    std::tm time{};
    time.tm_year = 2026 - 1900;
    time.tm_mon = 9;
    time.tm_mday = 5;
    time.tm_hour = 21;
    time.tm_min = 7;
    time.tm_sec = 3;
    time.tm_wday = 1;
    time.tm_yday = 277;
    return time;
}

std::string Ours(const char* format, const std::tm& time)
{
    char buffer[128];
    const size_t length = FeatherOS::LibC::FormatTime(buffer, sizeof(buffer), format, time);
    return std::string(buffer, length);
}

std::string Host(const char* format, const std::tm& time)
{
    char buffer[128];
    const size_t length = std::strftime(buffer, sizeof(buffer), format, &time);
    return std::string(buffer, length);
}

} // namespace

TEST_CASE("strftime matches the host's C locale for each conversion")
{
    std::tm time = Sample();
    for (const char* format :
         {"%a %A %b %B %h", "%c", "%C %y %Y", "%d %e %j", "%D %F %T %R %r", "%H %I %M %S %p",
          "%m %u %w", "%x %X", "100%% %n%t", "plain text"})
    {
        INFO(format);
        REQUIRE(Ours(format, time) == Host(format, time));
    }
    time.tm_hour = 0;
    time.tm_wday = 0;
    REQUIRE(Ours("%I %p %u %w", time) == Host("%I %p %u %w", time));
    time.tm_hour = 12;
    REQUIRE(Ours("%I %p", time) == Host("%I %p", time));
}

TEST_CASE("strftime reports UTC and returns 0 when the result does not fit")
{
    const std::tm time = Sample();
    REQUIRE(Ours("%z %Z", time) == "+0000 UTC");
    char small[8];
    REQUIRE(FeatherOS::LibC::FormatTime(small, sizeof(small), "%Y-%m-%d", time) == 0);
    REQUIRE(small[0] == '\0');
    REQUIRE(FeatherOS::LibC::FormatTime(small, sizeof(small), "%H:%M", time) == 5);
    REQUIRE(std::string(small) == "21:07");
}
