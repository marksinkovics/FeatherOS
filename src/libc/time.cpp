#include "libc/syscall_result.h"
#include "libc/time_format.h"
#include "user/calendar.h"
#include "user/syscall.h"

#include <errno.h>
#include <stdint.h>
#include <time.h>

namespace
{

constexpr int64_t SecondsPerDay = 86400;

struct tm shared_time;
char shared_text[32];

} // namespace

time_t time(time_t* result)
{
    const uint64_t seconds = FeatherOS::User::ReadClock();
    if (FeatherOS::User::IsSyscallError(seconds))
    {
        errno = EIO;
        return -1;
    }
    if (result != nullptr)
    {
        *result = static_cast<time_t>(seconds);
    }
    return static_cast<time_t>(seconds);
}

struct tm* gmtime_r(const time_t* seconds, struct tm* result)
{
    if (*seconds < 0)
    {
        errno = EOVERFLOW;
        return nullptr;
    }
    const auto unix_seconds = static_cast<uint64_t>(*seconds);
    const FeatherOS::User::CalendarTime calendar =
        FeatherOS::User::CalendarFromUnixSeconds(unix_seconds);
    result->tm_sec = static_cast<int>(calendar.second);
    result->tm_min = static_cast<int>(calendar.minute);
    result->tm_hour = static_cast<int>(calendar.hour);
    result->tm_mday = static_cast<int>(calendar.day);
    result->tm_mon = static_cast<int>(calendar.month) - 1;
    result->tm_year = static_cast<int>(calendar.year) - 1900;
    result->tm_wday = static_cast<int>(FeatherOS::User::WeekdayFromUnixSeconds(unix_seconds));
    result->tm_yday = static_cast<int>(unix_seconds / SecondsPerDay -
                                       FeatherOS::User::DaysFromCivil(calendar.year, 1, 1));
    result->tm_isdst = 0;
    return result;
}

struct tm* gmtime(const time_t* seconds)
{
    return gmtime_r(seconds, &shared_time);
}

struct tm* localtime_r(const time_t* seconds, struct tm* result)
{
    return gmtime_r(seconds, result);
}

struct tm* localtime(const time_t* seconds)
{
    return gmtime_r(seconds, &shared_time);
}

time_t timegm(struct tm* time)
{
    int64_t year = static_cast<int64_t>(time->tm_year) + 1900 + time->tm_mon / 12;
    int64_t month = time->tm_mon % 12;
    if (month < 0)
    {
        month += 12;
        --year;
    }
    if (year < 1970)
    {
        errno = EOVERFLOW;
        return -1;
    }
    const auto days = static_cast<int64_t>(FeatherOS::User::DaysFromCivil(
                          static_cast<uint32_t>(year), static_cast<uint32_t>(month + 1), 1)) +
                      time->tm_mday - 1;
    const time_t seconds = days * SecondsPerDay + static_cast<int64_t>(time->tm_hour) * 3600 +
                           static_cast<int64_t>(time->tm_min) * 60 + time->tm_sec;
    if (seconds < 0)
    {
        errno = EOVERFLOW;
        return -1;
    }
    gmtime_r(&seconds, time);
    return seconds;
}

time_t mktime(struct tm* time)
{
    return timegm(time);
}

size_t strftime(char* buffer, size_t size, const char* format, const struct tm* time)
{
    return FeatherOS::LibC::FormatTime(buffer, size, format, *time);
}

// The fixed format asctime has always had, as "Sun Sep 16 01:03:52 1973\n".
char* asctime(const struct tm* time)
{
    FeatherOS::LibC::FormatTime(shared_text, sizeof(shared_text), "%a %b %e %H:%M:%S %Y\n", *time);
    return shared_text;
}

char* ctime(const time_t* seconds)
{
    struct tm calendar;
    return localtime_r(seconds, &calendar) == nullptr ? nullptr : asctime(&calendar);
}

int clock_gettime(clockid_t clock, struct timespec* result)
{
    if (clock == CLOCK_REALTIME)
    {
        const time_t seconds = time(nullptr);
        if (seconds < 0)
        {
            return -1;
        }
        *result = {seconds, 0};
        return 0;
    }
    if (clock == CLOCK_MONOTONIC)
    {
        const uint64_t milliseconds = FeatherOS::User::ReadUptime();
        if (FeatherOS::User::IsSyscallError(milliseconds))
        {
            errno = EIO;
            return -1;
        }
        *result = {static_cast<time_t>(milliseconds / 1000),
                   static_cast<long>(milliseconds % 1000) * 1000000};
        return 0;
    }
    errno = EINVAL;
    return -1;
}

int nanosleep(const struct timespec* duration, struct timespec* remaining)
{
    if (duration->tv_sec < 0 || duration->tv_nsec < 0 || duration->tv_nsec > 999999999)
    {
        errno = EINVAL;
        return -1;
    }
    // Rounded up to whole milliseconds, slept at most a day per syscall.
    constexpr uint64_t MaximumStep = 86400 * 1000;
    uint64_t milliseconds = static_cast<uint64_t>(duration->tv_sec) * 1000 +
                            (static_cast<uint64_t>(duration->tv_nsec) + 999999) / 1000000;
    while (milliseconds > 0)
    {
        const uint64_t step = milliseconds < MaximumStep ? milliseconds : MaximumStep;
        FeatherOS::User::Sleep(step);
        milliseconds -= step;
    }
    if (remaining != nullptr)
    {
        *remaining = {0, 0};
    }
    return 0;
}
