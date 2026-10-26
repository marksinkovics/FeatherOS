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

bool IsLeapYear(uint32_t year);
uint32_t DaysInMonth(uint32_t year, uint32_t month);

// Days since 1970-01-01, using Howard Hinnant's days_from_civil algorithm.
uint64_t DaysFromCivil(uint32_t year, uint32_t month, uint32_t day);

bool UnixSecondsFromCalendar(const CalendarTime& time, uint64_t* seconds);

// Inverse of DaysFromCivil (Howard Hinnant's civil_from_days).
CalendarTime CalendarFromUnixSeconds(uint64_t seconds);

// Writes "YYYY-MM-DD HH:MM:SS UTC"; years beyond 9999 are not supported.
bool FormatCalendarTime(const CalendarTime& time, char (&buffer)[FormattedCalendarTimeLength]);

// "Wed Sep 30 12:59:59 PM" plus a terminator.
constexpr uint32_t ShortCalendarTimeLength = 23;

// 0 is Sunday; 1970-01-01 was a Thursday.
uint32_t WeekdayFromUnixSeconds(uint64_t seconds);

// Writes "Mon Oct 19 9:36:05 PM": weekday, month, day, and a 12-hour clock.
void FormatShortCalendarTime(uint64_t seconds, char (&buffer)[ShortCalendarTimeLength]);

} // namespace FeatherOS::User

#endif
