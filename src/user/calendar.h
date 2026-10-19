#ifndef FEATHEROS_USER_CALENDAR_H
#define FEATHEROS_USER_CALENDAR_H

#include <stdint.h>

// UTC calendar conversions shared by the kernel and user programs. Supports dates from
// 1970-01-01 onward in the proleptic Gregorian calendar.
namespace FeatherOS::User
{

struct CalendarTime
{
    uint32_t year;
    uint32_t month;
    uint32_t day;
    uint32_t hour;
    uint32_t minute;
    uint32_t second;
};

// "YYYY-MM-DD HH:MM:SS UTC" plus a terminator.
constexpr uint32_t FormattedCalendarTimeLength = 24;

inline bool IsLeapYear(uint32_t year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

inline uint32_t DaysInMonth(uint32_t year, uint32_t month)
{
    constexpr uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && IsLeapYear(year) ? 29 : days[month - 1];
}

// Days since 1970-01-01, using Howard Hinnant's days_from_civil algorithm.
inline uint64_t DaysFromCivil(uint32_t year, uint32_t month, uint32_t day)
{
    const uint64_t shifted_year = month <= 2 ? year - 1 : year;
    const uint64_t era = shifted_year / 400;
    const uint64_t year_of_era = shifted_year - era * 400;
    const uint64_t day_of_year = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const uint64_t day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;
}

inline bool UnixSecondsFromCalendar(const CalendarTime& time, uint64_t* seconds)
{
    if (seconds == nullptr || time.year < 1970 || time.month < 1 || time.month > 12 ||
        time.day < 1 || time.day > DaysInMonth(time.year, time.month) || time.hour > 23 ||
        time.minute > 59 || time.second > 59)
    {
        return false;
    }
    *seconds = DaysFromCivil(time.year, time.month, time.day) * 86400 + time.hour * 3600 +
               time.minute * 60 + time.second;
    return true;
}

// Inverse of DaysFromCivil (Howard Hinnant's civil_from_days).
inline CalendarTime CalendarFromUnixSeconds(uint64_t seconds)
{
    const uint64_t days = seconds / 86400;
    const uint64_t second_of_day = seconds % 86400;
    const uint64_t shifted_days = days + 719468;
    const uint64_t era = shifted_days / 146097;
    const uint64_t day_of_era = shifted_days - era * 146097;
    const uint64_t year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const uint64_t day_of_year =
        day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const uint64_t shifted_month = (5 * day_of_year + 2) / 153;
    const uint32_t month =
        static_cast<uint32_t>(shifted_month < 10 ? shifted_month + 3 : shifted_month - 9);
    const uint64_t year = year_of_era + era * 400 + (month <= 2 ? 1 : 0);

    CalendarTime time;
    time.year = static_cast<uint32_t>(year);
    time.month = month;
    time.day = static_cast<uint32_t>(day_of_year - (153 * shifted_month + 2) / 5 + 1);
    time.hour = static_cast<uint32_t>(second_of_day / 3600);
    time.minute = static_cast<uint32_t>(second_of_day / 60 % 60);
    time.second = static_cast<uint32_t>(second_of_day % 60);
    return time;
}

// Writes "YYYY-MM-DD HH:MM:SS UTC"; years beyond 9999 are not supported.
inline bool FormatCalendarTime(const CalendarTime& time,
                               char (&buffer)[FormattedCalendarTimeLength])
{
    if (time.year > 9999)
    {
        return false;
    }
    const uint32_t fields[] = {time.year, time.month,  time.day,
                               time.hour, time.minute, time.second};
    const uint32_t widths[] = {4, 2, 2, 2, 2, 2};
    const char separators[] = {'-', '-', ' ', ':', ':', ' '};
    uint32_t length = 0;
    for (uint32_t field = 0; field < 6; ++field)
    {
        uint32_t value = fields[field];
        for (uint32_t digit = widths[field]; digit > 0; --digit)
        {
            buffer[length + digit - 1] = static_cast<char>('0' + value % 10);
            value /= 10;
        }
        length += widths[field];
        buffer[length++] = separators[field];
    }
    buffer[length++] = 'U';
    buffer[length++] = 'T';
    buffer[length++] = 'C';
    buffer[length] = '\0';
    return true;
}

} // namespace FeatherOS::User

#endif
