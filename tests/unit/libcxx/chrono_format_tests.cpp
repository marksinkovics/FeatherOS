#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <format>
#include <random>
#include <string>

#include "libcxx/include/chrono"
#include "libcxx/include/format"
#include "libcxx/include/string"

namespace
{

namespace fstd = featheros_std;
namespace ours = featheros_std::chrono;
namespace host = std::chrono;
using namespace fstd::chrono_literals;

std::string Text(const fstd::string& text)
{
    return {text.data(), text.size()};
}

template <class... Args> std::string Ours(fstd::format_string<Args...> pattern, Args&&... args)
{
    return Text(fstd::format(pattern, std::forward<Args>(args)...));
}

// Each pattern formats ours with our library and host with the host's, which is the reference.
template <class Ours, class Host>
void CheckPatterns(std::initializer_list<const char*> patterns, const Ours& ours, const Host& host)
{
    for (const char* pattern : patterns)
    {
        INFO(pattern);
        REQUIRE(Text(fstd::vformat(pattern, fstd::make_format_args(ours))) ==
                std::vformat(pattern, std::make_format_args(host)));
    }
}

template <size_t Size> struct Pattern
{
    char text[Size];
    constexpr Pattern(const char (&value)[Size])
    {
        for (size_t index = 0; index != Size; ++index)
        {
            text[index] = value[index];
        }
    }
    constexpr fstd::string_view View() const
    {
        return {text, Size - 1};
    }
};

template <Pattern Text, class... Args>
constexpr bool Accepted = requires {
    typename std::integral_constant<
        int, (fstd::__detail::CheckFormatString<Args...>(Text.View()), 0)>;
};

using Seconds = ours::seconds;
using Date = ours::year_month_day;
using Time = ours::hh_mm_ss<ours::milliseconds>;
using Instant = ours::sys_time<ours::milliseconds>;

static_assert(fstd::formattable<ours::seconds, char> &&
              fstd::formattable<ours::duration<double, fstd::milli>, char> &&
              fstd::formattable<ours::sys_seconds, char> && fstd::formattable<ours::sys_days, char> &&
              fstd::formattable<Time, char> && fstd::formattable<ours::day, char> &&
              fstd::formattable<ours::month, char> && fstd::formattable<ours::year, char> &&
              fstd::formattable<ours::weekday, char> && fstd::formattable<Date, char>);
static_assert(!fstd::formattable<ours::steady_clock::time_point, char> &&
              !fstd::formattable<ours::duration<bool>, char>);

static_assert(Accepted<"{}", Seconds> && Accepted<"{:%H:%M:%S}", Seconds> &&
              Accepted<"{:*^12%T}", Seconds> && Accepted<"{:>{}%Q%q}", Seconds, int> &&
              Accepted<"{:%j days %n%t%%}", Seconds> && Accepted<"{:.3}", ours::duration<double>> &&
              Accepted<"{:.{}%Q}", ours::duration<float>, int> && Accepted<"{:L%OH %EX}", Seconds> &&
              Accepted<"{:%F %T %Z %z %Ez %Oz %c}", Instant> &&
              Accepted<"{:%a %A %u %w %U %W %V %G %g %j}", Date> && Accepted<"{:%e}", ours::day> &&
              Accepted<"{:%B %Om}", ours::month> && Accepted<"{:%C %Ey %EY}", ours::year> &&
              Accepted<"{:%Ou}", ours::weekday> && Accepted<"{:%r %p %I}", Time>);
// Fields the type does not have, precisions on integers, literal text first, unknown or
// unmodifiable conversions, braces and a conversion cut off.
static_assert(!Accepted<"{:%Y}", Seconds> && !Accepted<"{:%a}", Seconds> &&
              !Accepted<"{:%z}", Seconds> && !Accepted<"{:%Q}", Instant> &&
              !Accepted<"{:%H}", Date> && !Accepted<"{:%d}", ours::month> &&
              !Accepted<"{:%j}", Time> && !Accepted<"{:%F}", ours::year> &&
              !Accepted<"{:.3}", Seconds> && !Accepted<"{:.3}", Instant> &&
              !Accepted<"{:x%H}", Seconds> && !Accepted<"{:%K}", Seconds> &&
              !Accepted<"{:%EH}", Seconds> && !Accepted<"{:%Od}", ours::month> &&
              !Accepted<"{:%H{}", Seconds> && !Accepted<"{:%}", Seconds> &&
              !Accepted<"{:%E}", Seconds> && !Accepted<"{:{}%H}", Seconds, double> &&
              !Accepted<"{:+%H}", Seconds> && !Accepted<"{:#%H}", Seconds>);

// The calendar is constexpr, as the standard asks.
static_assert(ours::sys_days(2024y / ours::February / 29).time_since_epoch().count() == 19782);
static_assert(Date(ours::sys_days(ours::days(-1))) == 1969y / ours::December / 31);
static_assert(ours::weekday(ours::sys_days(1970y / 1 / 1)) == ours::Thursday);
static_assert(ours::Sunday - ours::Monday == ours::days(6) && ours::weekday(7) == ours::Sunday &&
              ours::Saturday + ours::days(1) == ours::Sunday);
static_assert(ours::December + ours::months(1) == ours::January &&
              ours::January - ours::February == ours::months(11));
static_assert((2024y / ours::January / 31) + ours::months(1) == 2024y / ours::February / 31 &&
              !((2024y / ours::February / 31).ok()) &&
              (2023y / ours::November / 5) + ours::months(-11) == 2022y / ours::December / 5);
static_assert(ours::sys_days(2023y / ours::February / 30) == ours::sys_days(2023y / 3 / 2));
static_assert(ours::year(2000).is_leap() && !ours::year(1900).is_leap() &&
              ours::year(2024).is_leap() && !ours::year(-32768).ok() && ours::year::min().ok());
static_assert(ours::February / 29 == ours::month_day(ours::February, 29d) &&
              (ours::February / 29).ok() && !(ours::February / 30).ok() &&
              29d / ours::February / 2023 == 2023y / ours::February / 29d);
static_assert(Time::fractional_width == 3 && ours::hh_mm_ss<ours::minutes>::fractional_width == 0 &&
              ours::hh_mm_ss<ours::duration<int, fstd::ratio<1, 3>>>::fractional_width == 6 &&
              ours::hh_mm_ss<ours::duration<long long, fstd::ratio<1, 1024>>>::fractional_width ==
                  10);
static_assert(Time(-(25h + 61min + 2s + 3ms)).hours() == 26h &&
              Time(-(25h + 61min + 2s + 3ms)).minutes() == 1min &&
              Time(-(25h + 61min + 2s + 3ms)).subseconds() == 3ms &&
              Time(-(25h + 61min + 2s + 3ms)).is_negative() &&
              Time(-(25h + 61min + 2s + 3ms)).to_duration() == -(25h + 61min + 2s + 3ms));
static_assert(ours::is_am(0h) && ours::is_pm(12h) && !ours::is_pm(24h) &&
              ours::make12(0h) == 12h && ours::make12(13h) == 1h && ours::make24(12h, false) == 0h &&
              ours::make24(1h, true) == 13h);

host::year_month_day HostDate(const Date& date)
{
    return {host::year(static_cast<int>(date.year())), host::month(unsigned(date.month())),
            host::day(unsigned(date.day()))};
}

template <class Rep, class Period>
auto HostDuration(ours::duration<Rep, Period> value)
{
    return host::duration<Rep, std::ratio<Period::num, Period::den>>(value.count());
}

template <class Duration> auto HostInstant(ours::sys_time<Duration> value)
{
    return host::sys_time<decltype(HostDuration(value.time_since_epoch()))>(
        HostDuration(value.time_since_epoch()));
}

constexpr std::initializer_list<const char*> DurationPatterns = {
    "{}",       "{:%H:%M:%S}", "{:%T}",     "{:%R}",        "{:%r}",     "{:%X|%EX}",
    "{:%I %p}", "{:%j}",       "{:%Q %q}",  "{:%Q%q|%S}",   "{:%OH%OM%OS%OI}", "{:%n%t%%%H}",
    "{:>16}",   "{:*^20%T}",   "{:<9%q}",   "{:L%T}",
};

template <class Rep, class Period>
void CheckDuration(Rep count, std::initializer_list<const char*> patterns = DurationPatterns)
{
    const ours::duration<Rep, Period> value(count);
    CheckPatterns(patterns, value, HostDuration(value));
}

TEST_CASE("Calendar conversions match the host's over a wide range of days", "[libcxx][chrono]")
{
    for (int count = -12'687'000; count <= 11'248'000; count += 997)
    {
        const Date ours_date{ours::sys_days(ours::days(count))};
        const host::year_month_day host_date{host::sys_days(host::days(count))};
        INFO(count);
        REQUIRE(HostDate(ours_date) == host_date);
        REQUIRE(ours::sys_days(ours_date).time_since_epoch().count() == count);
        REQUIRE(ours::weekday(ours::sys_days(ours::days(count))).c_encoding() ==
                host::weekday(host::sys_days(host::days(count))).c_encoding());
    }
    for (int month = -30; month <= 30; ++month)
    {
        const Date moved = (2024y / ours::March / 31) + ours::months(month);
        REQUIRE(HostDate(moved) == host::year(2024) / host::March / 31 + host::months(month));
        REQUIRE(moved.ok() == HostDate(moved).ok());
        REQUIRE(ours::sys_days(moved).time_since_epoch().count() ==
                host::sys_days(HostDate(moved)).time_since_epoch().count());
    }
}

TEST_CASE("Durations format as the host's do", "[libcxx][chrono]")
{
    for (const long long count : {0LL, 1LL, -1LL, 59LL, 3599LL, 3600LL, 86399LL, 86400LL,
                                  -90061LL, 1'000'000'007LL})
    {
        INFO(count);
        CheckDuration<long long, fstd::ratio<1>>(count);
        CheckDuration<long long, fstd::milli>(count);
        CheckDuration<long long, fstd::nano>(count * 1'000'003);
        CheckDuration<int, fstd::ratio<60>>(static_cast<int>(count % 100'000));
        CheckDuration<long long, fstd::ratio<3600>>(count % 100'000);
        CheckDuration<int, fstd::ratio<86400>>(static_cast<int>(count % 1000));
        CheckDuration<int, fstd::ratio<1, 3>>(static_cast<int>(count % 1'000'000));
        CheckDuration<long long, fstd::ratio<1, 1024>>(count);
        CheckDuration<long long, fstd::ratio<2, 3>>(count);
        CheckDuration<unsigned, fstd::ratio<1>>(static_cast<unsigned>(count < 0 ? -count : count));
    }
    // Every period with a name of its own, and the rest in brackets. libc++ keeps %j in an int.
    const std::initializer_list<const char*> named = {"{}", "{:%Q|%q}", "{:%T}"};
    CheckDuration<int, fstd::atto>(5, named);
    CheckDuration<int, fstd::femto>(5, named);
    CheckDuration<int, fstd::pico>(5, named);
    CheckDuration<int, fstd::micro>(5, named);
    CheckDuration<int, fstd::centi>(5, named);
    CheckDuration<int, fstd::deci>(5, named);
    CheckDuration<int, fstd::deca>(5, named);
    CheckDuration<int, fstd::hecto>(5, named);
    CheckDuration<int, fstd::kilo>(5, named);
    CheckDuration<int, fstd::mega>(5, named);
    CheckDuration<int, fstd::giga>(5, named);
    CheckDuration<int, fstd::tera>(5, named);
    CheckDuration<int, fstd::peta>(5, named);
    CheckDuration<int, fstd::exa>(5, named);
    CheckDuration<int, fstd::ratio<604800>>(2);
    CheckDuration<int, fstd::ratio<60, 7>>(-2);
    REQUIRE(Ours("{:%j}", ours::duration<int, fstd::peta>(5)) == "57870370370");
    // Floating point: the shortest text, and whole seconds when the period has no fraction.
    CheckPatterns({"{}", "{:%Q}", "{:%Q %q}", "{:%T}", "{:%H %M %S}"},
                  ours::duration<double>(3661.25), host::duration<double>(3661.25));
    CheckPatterns({"{}", "{:%Q}", "{:%j %T}"}, ours::duration<float, fstd::ratio<60>>(-1e4F),
                  host::duration<float, std::ratio<60>>(-1e4F));
    CheckPatterns({"{}", "{:%Q}"}, ours::duration<double, fstd::milli>(1e20),
                  host::duration<double, std::milli>(1e20));
}

TEST_CASE("Durations diverge from libc++ where libc++ departs from the standard",
          "[libcxx][chrono]")
{
    // Chrono types align left by default, as non-arithmetic types do; libc++ aligns right.
    REQUIRE(Ours("[{:6}]", 5s) == "[5s    ]");
    REQUIRE(Ours("[{:9%T}]", -5s) == "[-00:00:05]");
    REQUIRE(Ours("[{:10%T}]", -5s) == "[-00:00:05 ]");
    REQUIRE(Ours("[{:>{}%T}|{:^{}}]", -5s, 11, 3min, 6) == "[  -00:00:05| 3min ]");
    REQUIRE(Ours("[{:{}}]", ours::sys_days(2024y / 1 / 2), 12) == "[2024-01-02  ]");
    // A precision is the count's, in fixed notation; libc++ drops the unit with one.
    REQUIRE(Ours("{:.3}", ours::duration<double>(1.5)) == "1.500s");
    REQUIRE(Ours("{:.0%Q}|{:.{}%Q%q}", ours::duration<double>(2.5),
                 ours::duration<float, fstd::milli>(-0.25F), 2) == "2|-0.25ms");
    REQUIRE(Ours("{:>8.1}", ours::duration<double, fstd::ratio<60>>(1.25)) == "  1.2min");
    // A floating-point fraction of a second is cut to its digits; libc++ rounds it.
    REQUIRE(Ours("{:%S}", ours::duration<double, fstd::milli>(1.9)) == "00.001");
    REQUIRE(Ours("{:%T}", ours::duration<float, fstd::milli>(-1500.5F)) == "-00:00:01.500");
}

TEST_CASE("Time points format as the host's do", "[libcxx][chrono]")
{
    const std::initializer_list<const char*> patterns = {
        "{}",
        "{:%F %T}",
        "{:%c|%Ec|%x|%Ex|%X|%EX|%D}",
        "{:%a %A %b %B %h %C %EC %y %Ey %Oy %Y %EY}",
        "{:%d %Od %e %Oe %j %m %Om %u %Ou %w %Ow}",
        "{:%U %OU %W %OW %V %OV %G %g}",
        "{:%H %OH %I %OI %M %OM %S %OS %p}",
        "{:%r %R %T}",
        "{:%z %Ez %Oz %Z}",
        "{:%n%t%%}",
        "{:*>40%F %T}",
        "{:<30}|",
    };
    std::mt19937_64 random(20261111);
    std::uniform_int_distribution<long long> seconds(-62'000'000'000LL, 253'000'000'000LL);
    for (int round = 0; round != 400; ++round)
    {
        const long long count = seconds(random);
        INFO(count);
        const ours::sys_seconds point{Seconds(count)};
        CheckPatterns(patterns, point, HostInstant(point));
        const ours::sys_time<ours::milliseconds> precise{ours::milliseconds(count * 1000 + 7)};
        CheckPatterns(patterns, precise, HostInstant(precise));
        const ours::sys_days whole{ours::days(static_cast<int>(count / 86400))};
        CheckPatterns(patterns, whole, HostInstant(whole));
    }
    // ISO weeks around year ends, which can belong to the year before or after.
    for (int year = 1998; year != 2031; ++year)
    {
        for (const unsigned day : {1U, 2U, 3U, 4U, 28U, 29U, 30U, 31U})
        {
            const auto month = day < 10 ? ours::January : ours::December;
            const ours::sys_days point{ours::year(year) / month / ours::day(day)};
            CheckPatterns({"{:%F %a %U %W %V %G %g %j}"}, point, HostInstant(point));
        }
    }
    const ours::sys_time<ours::nanoseconds> nano{ours::nanoseconds(-1)};
    CheckPatterns({"{}", "{:%S}"}, nano, HostInstant(nano));
    const ours::sys_time<ours::minutes> minute{ours::minutes(27'000'000)};
    CheckPatterns({"{}", "{:%T}"}, minute, HostInstant(minute));
    const ours::sys_time<ours::hours> hour{ours::hours(-1)};
    CheckPatterns({"{}"}, hour, HostInstant(hour));
    const ours::sys_time<ours::duration<long long, fstd::ratio<1, 3>>> third{
        ours::duration<long long, fstd::ratio<1, 3>>(10)};
    CheckPatterns({"{}"}, third, HostInstant(third));
}

TEST_CASE("Calendar types and hh_mm_ss format as the host's do", "[libcxx][chrono]")
{
    for (unsigned value : {0U, 1U, 9U, 31U, 32U, 99U, 100U, 255U})
    {
        CheckPatterns({"{}", "{:%d|%e|%Od|%Oe}", "{:>5}"}, ours::day(value), host::day(value));
    }
    for (unsigned value = 0; value != 15; ++value)
    {
        CheckPatterns({"{}", "{:%m|%Om}", "{:*^24}"}, ours::month(value), host::month(value));
        if (value >= 1 && value <= 12)
        {
            CheckPatterns({"{:%b|%B|%h}"}, ours::month(value), host::month(value));
        }
    }
    for (int value : {-32768, -32767, -1000, -101, -100, -99, -5, -1, 0, 5, 99, 199, 1999, 2024,
                      12345, 32767})
    {
        CheckPatterns({"{}", "{:%Y|%y|%Ey|%Oy|%C}", "{:>12}"}, ours::year(value),
                      host::year(value));
        // The C locale's %EY and %EC are %Y and %C. libc++ takes them from strftime, which pads a
        // negative year to fewer digits and divides it toward zero.
        REQUIRE(Ours("{:%EY %EC}", ours::year(value)) == Ours("{:%Y %C}", ours::year(value)));
    }
    for (unsigned value = 0; value != 10; ++value)
    {
        CheckPatterns({"{}", "{:%u|%Ou|%w|%Ow}", "{:-^28}"}, ours::weekday(value),
                      host::weekday(value));
        if (value <= 7)
        {
            CheckPatterns({"{:%a|%A}"}, ours::weekday(value), host::weekday(value));
        }
    }
    for (const Date date : {2024y / 2 / 29, 2023y / 2 / 29, 2024y / 13 / 1, 2024y / 0 / 1,
                            2024y / 4 / 31, 2024y / 4 / 0, ours::year(-32768) / 1 / 1,
                            ours::year(-32767) / 1 / 1, 32767y / 12 / 31, 0y / 1 / 1})
    {
        CheckPatterns({"{}", "{:%F|%D|%x|%Y %m %d %e %C %y}", "{:>32}"}, date, HostDate(date));
        if (date.ok())
        {
            CheckPatterns({"{:%a %A %b %B %j %u %w %U %W %V %g}"}, date, HostDate(date));
        }
    }
    // %G pads to four digits after the sign, as %Y does; libc++ counts the sign as one of them.
    REQUIRE(Ours("{:%G|%Y}", 0y / 1 / 1) == "-0001|0000");
    REQUIRE(Ours("{:%G}", 2024y / 2 / 29) == "2024");
    for (const ours::milliseconds value : {0ms, 1ms, 999ms, 59'999ms, 3'600'000ms, 43'200'000ms,
                                           86'399'999ms, -1ms, -45'296'789ms})
    {
        const Time ours_time(value);
        const host::hh_mm_ss<host::milliseconds> host_time(host::milliseconds(value.count()));
        CheckPatterns({"{}", "{:%H:%M:%S}", "{:%T|%R|%r|%X}", "{:%I %p}", "{:*>16}"}, ours_time,
                      host_time);
    }
    CheckPatterns({"{}", "{:%T}"}, ours::hh_mm_ss<ours::seconds>(Seconds(86399)),
                  host::hh_mm_ss<host::seconds>(host::seconds(86399)));
    CheckPatterns({"{}"}, ours::hh_mm_ss<ours::duration<int, fstd::ratio<1, 3>>>(
                              ours::duration<int, fstd::ratio<1, 3>>(-7)),
                  host::hh_mm_ss<host::duration<int, std::ratio<1, 3>>>(
                      host::duration<int, std::ratio<1, 3>>(-7)));
}

} // namespace
