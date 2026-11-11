// Programs see the library's headers as the system headers they are; the host tests build this
// file against their own copy, in a namespace of its own beside the host's library.
#ifdef FEATHEROS_HOST_TEST
#include "libcxx/include/charconv"
#include "libcxx/include/chrono"
#include "libcxx/include/format"
#else
#include <charconv>
#include <chrono>
#include <format>
#endif

// This is the standard library, so it defines what std declares.
// NOLINTBEGIN(bugprone-std-namespace-modification)
namespace FEATHEROS_STD_NAMESPACE::__detail
{

namespace
{

constexpr const char* WeekdayNames[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                        "Thursday", "Friday", "Saturday"};
constexpr const char* MonthNames[] = {"January",   "February", "March",    "April",
                                      "May",       "June",     "July",     "August",
                                      "September", "October",  "November", "December"};

struct Suffix
{
    intmax_t num;
    intmax_t den;
    const char* text;
};

constexpr Suffix Suffixes[] = {
    {1, 1'000'000'000'000'000'000, "as"},
    {1, 1'000'000'000'000'000, "fs"},
    {1, 1'000'000'000'000, "ps"},
    {1, 1'000'000'000, "ns"},
    {1, 1'000'000, "µs"},
    {1, 1'000, "ms"},
    {1, 100, "cs"},
    {1, 10, "ds"},
    {1, 1, "s"},
    {10, 1, "das"},
    {100, 1, "hs"},
    {1'000, 1, "ks"},
    {1'000'000, 1, "Ms"},
    {1'000'000'000, 1, "Gs"},
    {1'000'000'000'000, 1, "Ts"},
    {1'000'000'000'000'000, 1, "Ps"},
    {1'000'000'000'000'000'000, 1, "Es"},
    {60, 1, "min"},
    {3600, 1, "h"},
    {86400, 1, "d"},
};

void WriteDecimal(FormatBuffer& out, unsigned long long value, size_t width, char pad)
{
    char digits[24];
    const char* const end = to_chars(digits, digits + sizeof(digits), value).ptr;
    const auto length = static_cast<size_t>(end - digits);
    if (length < width)
    {
        out.Fill(width - length, &pad, 1);
    }
    out.Write(digits, length);
}

void WriteSigned(FormatBuffer& out, long long value, size_t width)
{
    if (value < 0)
    {
        out.Put('-');
    }
    const auto bits = static_cast<unsigned long long>(value);
    WriteDecimal(out, value < 0 ? 0ULL - bits : bits, width, '0');
}

unsigned long long Magnitude(long long value)
{
    const auto bits = static_cast<unsigned long long>(value);
    return value < 0 ? 0ULL - bits : bits;
}

long long FloorDivide(long long value, long long divisor)
{
    const long long quotient = value / divisor;
    return quotient * divisor > value ? quotient - 1 : quotient;
}

class ChronoWriter
{
public:
    ChronoWriter(format_context& context, const FormatSpec& spec, const ChronoParts& parts)
        : m_context(context), m_out(context.out().Buffer()), m_spec(spec), m_parts(parts)
    {
    }

    void Write(string_view chrono_specs)
    {
        if (chrono_specs.empty())
        {
            WriteDefault();
            return;
        }
        if (m_parts.negative)
        {
            m_out.Put('-');
        }
        const char* cursor = chrono_specs.data();
        const char* const end = cursor + chrono_specs.size();
        while (cursor != end)
        {
            if (*cursor != '%')
            {
                m_out.Put(*cursor++);
                continue;
            }
            ++cursor;
            char modifier = '\0';
            if (*cursor == 'E' || *cursor == 'O')
            {
                modifier = *cursor++;
            }
            Convert(modifier, *cursor++);
        }
    }

private:
    bool IsDateKind() const
    {
        return m_parts.kind == ChronoKind::Date || m_parts.kind == ChronoKind::TimePoint;
    }

    bool DateIsValid() const
    {
        return m_parts.year != -32768 && m_parts.month >= 1 && m_parts.month <= 12 &&
               m_parts.day >= 1 && m_parts.day <= LastDayOfMonth(m_parts.year, m_parts.month);
    }

    void RequireDate(const char* message) const
    {
        if (!DateIsValid())
        {
            FormatError(message);
        }
    }

    long long DayCount() const
    {
        return DaysFromCivil(m_parts.year, m_parts.month, m_parts.day);
    }

    // c_encoding: Sunday is 0. A date's is found from the date, which must then be valid.
    unsigned Weekday() const
    {
        if (!IsDateKind())
        {
            return m_parts.weekday;
        }
        RequireDate("formatting a weekday needs a valid date");
        return static_cast<unsigned>(((DayCount() + 4) % 7 + 7) % 7);
    }

    unsigned DayOfYear() const
    {
        RequireDate("formatting a day of the year needs a valid date");
        return static_cast<unsigned>(DayCount() - DaysFromCivil(m_parts.year, 1, 1));
    }

    struct IsoWeek
    {
        int year;
        unsigned week;
    };

    // ISO 8601: weeks start on Monday, and week 1 holds the year's first Thursday.
    IsoWeek Iso() const
    {
        RequireDate("formatting a week of the year needs a valid date");
        const long long count = DayCount();
        const unsigned weekday = Weekday();
        const long long thursday = count + 3 - (weekday == 0 ? 6 : weekday - 1);
        const int iso_year = CivilFromDays(static_cast<int>(thursday)).year;
        return {iso_year,
                static_cast<unsigned>((thursday - DaysFromCivil(iso_year, 1, 1)) / 7 + 1)};
    }

    // A duration's hours run past a day; its days are %j.
    unsigned Hour24() const
    {
        if (m_parts.kind == ChronoKind::Duration)
        {
            return static_cast<unsigned>(m_parts.hours % 24);
        }
        if (m_parts.hours >= 24)
        {
            FormatError("formatting an hour needs a time of day under 24 hours");
        }
        return static_cast<unsigned>(m_parts.hours);
    }

    void WriteSeconds(bool fraction)
    {
        WriteDecimal(m_out, m_parts.seconds, 2, '0');
        if (fraction && m_parts.fractional_width != 0)
        {
            m_out.Put('.');
            WriteDecimal(m_out, m_parts.subseconds, m_parts.fractional_width, '0');
        }
    }

    void WriteWeekdayName(bool full)
    {
        const unsigned weekday = Weekday();
        if (weekday > 6)
        {
            FormatError("formatting a weekday name needs a valid weekday");
        }
        const string_view name = WeekdayNames[weekday];
        m_out.Write(full ? name : name.substr(0, 3));
    }

    void WriteMonthName(bool full)
    {
        if (m_parts.month < 1 || m_parts.month > 12)
        {
            FormatError("formatting a month name needs a valid month");
        }
        const string_view name = MonthNames[m_parts.month - 1];
        m_out.Write(full ? name : name.substr(0, 3));
    }

    void WriteYear(long long year)
    {
        WriteSigned(m_out, year, 4);
    }

    void WriteCentury(long long year)
    {
        const long long century = FloorDivide(year, 100);
        if (century < 0)
        {
            m_out.Put('-');
            WriteDecimal(m_out, Magnitude(century), 1, '0');
        }
        else
        {
            WriteDecimal(m_out, static_cast<unsigned long long>(century), 2, '0');
        }
    }

    void WriteTwoDigitYear(long long year)
    {
        WriteDecimal(m_out, Magnitude(year) % 100, 2, '0');
    }

    void WriteCount()
    {
        FormatSpec count_spec;
        if (m_spec.precision >= 0 || m_spec.precision_argument >= 0)
        {
            count_spec.precision = m_spec.precision;
            count_spec.precision_argument = m_spec.precision_argument;
            count_spec.type = 'f';
        }
        visit_format_arg(
            [&](auto value)
            {
                using T = decltype(value);
                if constexpr (is_same_v<T, unsigned long long>)
                {
                    FormatIntegerValue(m_context, count_spec, value, false);
                }
                else if constexpr (is_same_v<T, float> || is_same_v<T, double> ||
                                   is_same_v<T, long double>)
                {
                    FormatFloatValue(m_context, count_spec, value);
                }
            },
            m_parts.count);
    }

    void WriteSuffix()
    {
        for (const Suffix& suffix : Suffixes)
        {
            if (suffix.num == m_parts.period_num && suffix.den == m_parts.period_den)
            {
                m_out.Write(string_view(suffix.text));
                return;
            }
        }
        m_out.Put('[');
        WriteSigned(m_out, m_parts.period_num, 1);
        if (m_parts.period_den != 1)
        {
            m_out.Put('/');
            WriteSigned(m_out, m_parts.period_den, 1);
        }
        m_out.Write(string_view("]s"));
    }

    void WriteDefault()
    {
        switch (m_parts.kind)
        {
        case ChronoKind::Duration:
            if (m_parts.negative)
            {
                m_out.Put('-');
            }
            WriteCount();
            WriteSuffix();
            break;
        case ChronoKind::TimePoint:
            Convert('\0', 'F');
            if (!m_parts.date_only)
            {
                m_out.Put(' ');
                Convert('\0', 'T');
            }
            break;
        case ChronoKind::TimeOfDay:
            if (m_parts.negative)
            {
                m_out.Put('-');
            }
            Convert('\0', 'T');
            break;
        case ChronoKind::Day:
            Convert('\0', 'd');
            if (m_parts.day < 1 || m_parts.day > 31)
            {
                m_out.Write(string_view(" is not a valid day"));
            }
            break;
        case ChronoKind::Month:
            if (m_parts.month >= 1 && m_parts.month <= 12)
            {
                Convert('\0', 'b');
            }
            else
            {
                WriteDecimal(m_out, m_parts.month, 1, '0');
                m_out.Write(string_view(" is not a valid month"));
            }
            break;
        case ChronoKind::Year:
            if (m_parts.year != -32768)
            {
                Convert('\0', 'Y');
            }
            else
            {
                WriteSigned(m_out, m_parts.year, 1);
                m_out.Write(string_view(" is not a valid year"));
            }
            break;
        case ChronoKind::Weekday:
            if (m_parts.weekday <= 6)
            {
                Convert('\0', 'a');
            }
            else
            {
                WriteDecimal(m_out, m_parts.weekday, 1, '0');
                m_out.Write(string_view(" is not a valid weekday"));
            }
            break;
        case ChronoKind::Date:
            Convert('\0', 'F');
            if (!DateIsValid())
            {
                m_out.Write(string_view(" is not a valid date"));
            }
            break;
        }
    }

    void Convert(char modifier, char conversion)
    {
        switch (conversion)
        {
        case 'a':
        case 'A':
            WriteWeekdayName(conversion == 'A');
            break;
        case 'b':
        case 'h':
        case 'B':
            WriteMonthName(conversion == 'B');
            break;
        case 'c':
            Convert('\0', 'a');
            m_out.Put(' ');
            Convert('\0', 'b');
            m_out.Put(' ');
            Convert('\0', 'e');
            m_out.Put(' ');
            Convert('\0', 'X');
            m_out.Put(' ');
            Convert('\0', 'Y');
            break;
        case 'C':
            WriteCentury(m_parts.year);
            break;
        case 'd':
            WriteDecimal(m_out, m_parts.day, 2, '0');
            break;
        case 'e':
            WriteDecimal(m_out, m_parts.day, 2, ' ');
            break;
        case 'D':
        case 'x':
            Convert('\0', 'm');
            m_out.Put('/');
            Convert('\0', 'd');
            m_out.Put('/');
            Convert('\0', 'y');
            break;
        case 'F':
            Convert('\0', 'Y');
            m_out.Put('-');
            Convert('\0', 'm');
            m_out.Put('-');
            Convert('\0', 'd');
            break;
        case 'g':
            WriteTwoDigitYear(Iso().year);
            break;
        case 'G':
            WriteYear(Iso().year);
            break;
        case 'H':
            WriteDecimal(m_out, Hour24(), 2, '0');
            break;
        case 'I':
        {
            const unsigned hour = Hour24() % 12;
            WriteDecimal(m_out, hour == 0 ? 12 : hour, 2, '0');
            break;
        }
        case 'j':
            if (m_parts.kind == ChronoKind::Duration)
            {
                WriteDecimal(m_out, m_parts.hours / 24, 1, '0');
            }
            else
            {
                WriteDecimal(m_out, DayOfYear() + 1, 3, '0');
            }
            break;
        case 'm':
            WriteDecimal(m_out, m_parts.month, 2, '0');
            break;
        case 'M':
            WriteDecimal(m_out, m_parts.minutes, 2, '0');
            break;
        case 'n':
            m_out.Put('\n');
            break;
        case 'p':
            m_out.Write(string_view(Hour24() < 12 ? "AM" : "PM"));
            break;
        case 'q':
            WriteSuffix();
            break;
        case 'Q':
            WriteCount();
            break;
        case 'r':
            Convert('\0', 'I');
            m_out.Put(':');
            Convert('\0', 'M');
            m_out.Put(':');
            WriteSeconds(false);
            m_out.Put(' ');
            Convert('\0', 'p');
            break;
        case 'R':
            Convert('\0', 'H');
            m_out.Put(':');
            Convert('\0', 'M');
            break;
        case 'S':
            WriteSeconds(true);
            break;
        case 't':
            m_out.Put('\t');
            break;
        case 'T':
            Convert('\0', 'R');
            m_out.Put(':');
            WriteSeconds(true);
            break;
        case 'u':
        {
            const unsigned weekday = Weekday();
            WriteDecimal(m_out, weekday == 0 ? 7 : weekday, 1, '0');
            break;
        }
        case 'U':
        {
            const unsigned day_of_year = DayOfYear();
            WriteDecimal(m_out, (day_of_year + 7 - Weekday()) / 7, 2, '0');
            break;
        }
        case 'V':
            WriteDecimal(m_out, Iso().week, 2, '0');
            break;
        case 'w':
            WriteDecimal(m_out, Weekday(), 1, '0');
            break;
        case 'W':
        {
            const unsigned day_of_year = DayOfYear();
            WriteDecimal(m_out, (day_of_year + 7 - (Weekday() + 6) % 7) / 7, 2, '0');
            break;
        }
        case 'X':
            Convert('\0', 'R');
            m_out.Put(':');
            WriteSeconds(false);
            break;
        case 'y':
            WriteTwoDigitYear(m_parts.year);
            break;
        case 'Y':
            WriteYear(m_parts.year);
            break;
        case 'z':
            m_out.Write(string_view(modifier == '\0' ? "+0000" : "+00:00"));
            break;
        case 'Z':
            m_out.Write(string_view("UTC"));
            break;
        default:
            m_out.Put('%');
            break;
        }
    }

    format_context& m_context;
    FormatBuffer& m_out;
    const FormatSpec& m_spec;
    const ChronoParts& m_parts;
};

struct ChronoState
{
    const FormatSpec* spec;
    string_view chrono_specs;
    const ChronoParts* parts;
};

} // namespace

void FormatChronoValue(format_context& context, const FormatSpec& spec, string_view chrono_specs,
                       const ChronoParts& parts)
{
    const ChronoState state = {&spec, chrono_specs, &parts};
    FormatPadded(
        context, spec,
        [](format_context& inner, const void* erased)
        {
            const auto& [inner_spec, inner_specs, inner_parts] =
                *static_cast<const ChronoState*>(erased);
            ChronoWriter(inner, *inner_spec, *inner_parts).Write(inner_specs);
        },
        &state);
}

} // namespace FEATHEROS_STD_NAMESPACE::__detail
// NOLINTEND(bugprone-std-namespace-modification)
