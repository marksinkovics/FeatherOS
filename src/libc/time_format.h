#ifndef FEATHEROS_LIBC_TIME_FORMAT_H
#define FEATHEROS_LIBC_TIME_FORMAT_H

#include <stddef.h>

namespace FeatherOS::LibC
{

namespace TimeFormatDetail
{

constexpr const char* WeekdayNames[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                        "Thursday", "Friday", "Saturday"};
constexpr const char* MonthNames[] = {"January",   "February", "March",    "April",
                                      "May",       "June",     "July",     "August",
                                      "September", "October",  "November", "December"};

class Output
{
public:
    Output(char* target, size_t target_size);
    void Put(char character);
    void Text(const char* text, size_t count = static_cast<size_t>(-1));
    void Number(long value, int width, char pad);
    size_t Finish();

private:
    char* buffer;
    size_t size;
    size_t length = 0;
    bool overflowed = false;
};

template <typename Time> void Convert(Output& output, char conversion, const Time& time)
{
    const int weekday = time.tm_wday >= 0 && time.tm_wday < 7 ? time.tm_wday : 0;
    const int month = time.tm_mon >= 0 && time.tm_mon < 12 ? time.tm_mon : 0;
    const int hour12 = time.tm_hour % 12 == 0 ? 12 : time.tm_hour % 12;
    switch (conversion)
    {
    case 'a':
        output.Text(WeekdayNames[weekday], 3);
        break;
    case 'A':
        output.Text(WeekdayNames[weekday]);
        break;
    case 'b':
    case 'h':
        output.Text(MonthNames[month], 3);
        break;
    case 'B':
        output.Text(MonthNames[month]);
        break;
    case 'c':
        Convert(output, 'a', time);
        output.Put(' ');
        Convert(output, 'b', time);
        output.Put(' ');
        Convert(output, 'e', time);
        output.Put(' ');
        Convert(output, 'T', time);
        output.Put(' ');
        Convert(output, 'Y', time);
        break;
    case 'C':
        output.Number((time.tm_year + 1900) / 100, 2, '0');
        break;
    case 'd':
        output.Number(time.tm_mday, 2, '0');
        break;
    case 'D':
        Convert(output, 'm', time);
        output.Put('/');
        Convert(output, 'd', time);
        output.Put('/');
        Convert(output, 'y', time);
        break;
    case 'e':
        output.Number(time.tm_mday, 2, ' ');
        break;
    case 'F':
        Convert(output, 'Y', time);
        output.Put('-');
        Convert(output, 'm', time);
        output.Put('-');
        Convert(output, 'd', time);
        break;
    case 'H':
        output.Number(time.tm_hour, 2, '0');
        break;
    case 'I':
        output.Number(hour12, 2, '0');
        break;
    case 'j':
        output.Number(time.tm_yday + 1, 3, '0');
        break;
    case 'm':
        output.Number(time.tm_mon + 1, 2, '0');
        break;
    case 'M':
        output.Number(time.tm_min, 2, '0');
        break;
    case 'n':
        output.Put('\n');
        break;
    case 'p':
        output.Text(time.tm_hour < 12 ? "AM" : "PM");
        break;
    case 'r':
        Convert(output, 'I', time);
        output.Put(':');
        Convert(output, 'M', time);
        output.Put(':');
        Convert(output, 'S', time);
        output.Put(' ');
        Convert(output, 'p', time);
        break;
    case 'R':
        Convert(output, 'H', time);
        output.Put(':');
        Convert(output, 'M', time);
        break;
    case 'S':
        output.Number(time.tm_sec, 2, '0');
        break;
    case 't':
        output.Put('\t');
        break;
    case 'T':
        Convert(output, 'H', time);
        output.Put(':');
        Convert(output, 'M', time);
        output.Put(':');
        Convert(output, 'S', time);
        break;
    case 'u':
        output.Number(weekday == 0 ? 7 : weekday, 1, '0');
        break;
    case 'w':
        output.Number(weekday, 1, '0');
        break;
    case 'x':
        Convert(output, 'D', time);
        break;
    case 'X':
        Convert(output, 'T', time);
        break;
    case 'y':
        output.Number((time.tm_year + 1900) % 100, 2, '0');
        break;
    case 'Y':
        output.Number(time.tm_year + 1900, 1, '0');
        break;
    // Every time is UTC.
    case 'z':
        output.Text("+0000");
        break;
    case 'Z':
        output.Text("UTC");
        break;
    case '%':
        output.Put('%');
        break;
    default:
        output.Put('%');
        output.Put(conversion);
        break;
    }
}

} // namespace TimeFormatDetail

// strftime for the "C" locale. Returns the length written, or 0 (with an empty buffer) if the
// result and its terminator do not fit. Time is struct tm, or anything with its fields.
template <typename Time>
size_t FormatTime(char* buffer, size_t size, const char* format, const Time& time)
{
    TimeFormatDetail::Output output(buffer, size);
    for (; *format != '\0'; ++format)
    {
        if (*format != '%')
        {
            output.Put(*format);
            continue;
        }
        ++format;
        if (*format == '\0')
        {
            output.Put('%');
            break;
        }
        TimeFormatDetail::Convert(output, *format, time);
    }
    return output.Finish();
}

} // namespace FeatherOS::LibC

#endif
