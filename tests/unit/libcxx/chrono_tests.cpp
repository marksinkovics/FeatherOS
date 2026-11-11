#include <catch2/catch_test_macros.hpp>

#include "libcxx/include/chrono"
#include "libcxx/include/ratio"

#include <time.h>

namespace
{

namespace fstd = featheros_std;
namespace chrono = featheros_std::chrono;
using namespace fstd::chrono_literals;

using Third = fstd::ratio<2, -6>;
static_assert(Third::num == -1 && Third::den == 3);
static_assert(fstd::is_same_v<Third::type, fstd::ratio<-1, 3>>);
static_assert(
    fstd::is_same_v<fstd::ratio_add<fstd::ratio<1, 3>, fstd::ratio<1, 6>>, fstd::ratio<1, 2>>);
static_assert(
    fstd::is_same_v<fstd::ratio_subtract<fstd::milli, fstd::micro>, fstd::ratio<999, 1'000'000>>);
static_assert(fstd::is_same_v<fstd::ratio_multiply<fstd::kilo, fstd::milli>, fstd::ratio<1>>);
static_assert(fstd::is_same_v<fstd::ratio_divide<fstd::nano, fstd::giga>,
                              fstd::ratio<1, 1'000'000'000'000'000'000>>);
// The cross products need more than 64 bits.
static_assert(fstd::ratio_less_v<fstd::ratio<INTMAX_MAX - 1, INTMAX_MAX>, fstd::ratio<1>>);
static_assert(fstd::ratio_greater_equal_v<fstd::exa, fstd::peta>);
static_assert(fstd::ratio_equal_v<fstd::ratio<3, 9>, fstd::ratio<1, 3>>);

static_assert(chrono::milliseconds(1500) == chrono::seconds(1) + 500ms);
static_assert(chrono::duration_cast<chrono::seconds>(1999ms).count() == 1);
static_assert(chrono::duration_cast<chrono::seconds>(-1999ms).count() == -1);
static_assert(chrono::floor<chrono::seconds>(-1999ms).count() == -2);
static_assert(chrono::ceil<chrono::seconds>(1001ms).count() == 2);
static_assert(chrono::round<chrono::seconds>(1500ms).count() == 2);
static_assert(chrono::round<chrono::seconds>(2500ms).count() == 2);
static_assert(chrono::round<chrono::seconds>(2501ms).count() == 3);
static_assert(chrono::abs(-3s) == 3s);
static_assert((1h - 59min) == 60s);
static_assert(fstd::is_same_v<decltype(1s + 1ms), chrono::milliseconds>);
static_assert(fstd::is_same_v<fstd::common_type_t<chrono::duration<int, fstd::ratio<1, 3>>,
                                                  chrono::duration<long, fstd::ratio<1, 2>>>,
                              chrono::duration<long, fstd::ratio<1, 6>>>);
static_assert((10s / 4s) == 2 && (10s % 4s) == 2s && (10s / 4).count() == 2 && (3 * 2min) == 6min);
static_assert((1s <=> 999ms) > 0);
static_assert(chrono::duration<double>(1500ms).count() == 1.5);
static_assert(!fstd::is_convertible_v<chrono::milliseconds, chrono::seconds>);
static_assert(fstd::is_convertible_v<chrono::seconds, chrono::milliseconds>);
static_assert(!fstd::is_constructible_v<chrono::seconds, double>);
static_assert(chrono::seconds::max().count() == INT64_MAX);
static_assert(chrono::duration_cast<chrono::days>(chrono::weeks(2)).count() == 14);

using SysMilliseconds = chrono::sys_time<chrono::milliseconds>;
static_assert(SysMilliseconds().time_since_epoch() == 0ms);
static_assert((chrono::sys_seconds(5s) + 250ms).time_since_epoch() == 5250ms);
static_assert(fstd::is_same_v<decltype(chrono::sys_seconds() + 1ms), SysMilliseconds>);
static_assert(fstd::is_same_v<decltype(1ms + chrono::sys_seconds()), SysMilliseconds>);
static_assert(SysMilliseconds(1500ms) - chrono::sys_seconds(1s) == 500ms);
static_assert((SysMilliseconds(1500ms) - 2s).time_since_epoch() == -500ms);
static_assert(chrono::sys_seconds(2s) == SysMilliseconds(2000ms));
static_assert(chrono::sys_seconds(2s) < SysMilliseconds(2001ms));
static_assert((chrono::sys_seconds(2s) <=> SysMilliseconds(1999ms)) > 0);
static_assert(chrono::time_point_cast<chrono::seconds>(SysMilliseconds(-1500ms)) ==
              chrono::sys_seconds(-1s));
static_assert(chrono::floor<chrono::seconds>(SysMilliseconds(-1500ms)) ==
              chrono::sys_seconds(-2s));
static_assert(chrono::ceil<chrono::seconds>(SysMilliseconds(1001ms)) == chrono::sys_seconds(2s));
static_assert(chrono::round<chrono::seconds>(SysMilliseconds(2500ms)) == chrono::sys_seconds(2s));
static_assert(chrono::sys_days(chrono::days(1)).time_since_epoch() == 24h);
static_assert(fstd::is_convertible_v<chrono::sys_seconds, SysMilliseconds>);
static_assert(!fstd::is_convertible_v<SysMilliseconds, chrono::sys_seconds>);
static_assert(fstd::is_same_v<fstd::common_type_t<chrono::sys_seconds, SysMilliseconds>,
                              SysMilliseconds>);
static_assert(SysMilliseconds::max().time_since_epoch() == chrono::milliseconds::max());
static_assert(chrono::is_clock_v<chrono::system_clock> && chrono::is_clock_v<chrono::steady_clock>);
static_assert(!chrono::is_clock_v<chrono::seconds>);
static_assert(chrono::steady_clock::is_steady && !chrono::system_clock::is_steady);
static_assert(fstd::is_same_v<chrono::high_resolution_clock, chrono::steady_clock>);

} // namespace

TEST_CASE("chrono durations at run time", "[libcxx][chrono]")
{
    chrono::milliseconds elapsed = 250ms;
    elapsed += 1s;
    elapsed *= 2;
    REQUIRE(elapsed.count() == 2500);
    REQUIRE((elapsed--).count() == 2500);
    REQUIRE((--elapsed).count() == 2498);
    elapsed %= 1s;
    REQUIRE(elapsed == 498ms);
    REQUIRE(-elapsed < chrono::milliseconds::zero());
    REQUIRE(chrono::duration_cast<chrono::microseconds>(elapsed).count() == 498'000);
    REQUIRE(chrono::duration_cast<chrono::nanoseconds>(chrono::duration<double>(0.5)).count() ==
            500'000'000);
}

TEST_CASE("chrono time points at run time", "[libcxx][chrono]")
{
    SysMilliseconds point(1000ms);
    point += 500ms;
    REQUIRE((point++).time_since_epoch() == 1500ms);
    REQUIRE((--point).time_since_epoch() == 1500ms);
    point -= 2s;
    REQUIRE(point.time_since_epoch() == -500ms);

    REQUIRE(chrono::system_clock::to_time_t(chrono::system_clock::from_time_t(1'700'000'000)) ==
            1'700'000'000);
    const auto before = chrono::system_clock::to_time_t(chrono::system_clock::now());
    const time_t host = time(nullptr);
    const auto after = chrono::system_clock::to_time_t(chrono::system_clock::now());
    REQUIRE(before <= host);
    REQUIRE(host <= after);

    const auto first = chrono::steady_clock::now();
    timespec pause{0, 2'000'000};
    nanosleep(&pause, nullptr);
    const auto second = chrono::steady_clock::now();
    REQUIRE(second - first >= 1ms);
}
