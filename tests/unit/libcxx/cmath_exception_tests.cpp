#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

#include "libcxx/include/cmath"
#include "libcxx/include/exception"
#include "libcxx/include/string_view"
#include "libcxx/include/type_traits"

namespace
{

namespace fstd = featheros_std;

template <class T> T Opaque(T value)
{
    volatile T copy = value;
    return copy;
}

bool SameBits(double left, double right)
{
    return std::isnan(left) ? std::isnan(right) : left == right && std::signbit(left) == std::signbit(right);
}

bool SameBits(float left, float right)
{
    return std::isnan(left) ? std::isnan(right) : left == right && std::signbit(left) == std::signbit(right);
}

void TerminateHandler() {}

struct Derived : fstd::exception
{
    [[nodiscard]] const char* what() const noexcept override { return "derived"; }
};

} // namespace

TEST_CASE("cmath overloads pick the argument's type", "[libcxx]")
{
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::sqrt(1.0f)), float>);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::sqrt(1.0)), double>);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::sqrt(1)), double>);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::pow(2.0f, 3)), double>);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::pow(2.0f, 3.0f)), float>);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::fmax(1.0f, 2.0)), double>);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::fma(1.0f, 2, 3.0f)), double>);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::frexp(1.0f, nullptr)), float>);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::hypot(1.0f, 2.0f, 3.0f)), float>);
    STATIC_REQUIRE(fstd::is_same_v<decltype(fstd::lerp(1, 2.0f, 0.5f)), double>);
    STATIC_REQUIRE(fstd::is_same_v<fstd::float_t, float>);

    REQUIRE(fstd::sqrt(Opaque(16)) == 4.0);
    REQUIRE(fstd::sqrt(Opaque(2.25f)) == 1.5f);
    REQUIRE(fstd::pow(Opaque(2), 10) == 1024.0);
    REQUIRE(fstd::atan2(Opaque(0.0f), -1) == std::atan2(0.0, -1.0));
    REQUIRE(fstd::fmod(Opaque(7), 4.5) == 2.5);
    REQUIRE(fstd::copysign(Opaque(3.0f), -0.0f) == -3.0f);
    REQUIRE(fstd::abs(Opaque(-2.5)) == 2.5);
}

TEST_CASE("cmath gives the C library's results", "[libcxx]")
{
    const double values[] = {0.0, -0.0, 0.5, -1.25, 2.0, 3.75, 1e-310, 1e300, -7.5e5,
                             std::numeric_limits<double>::infinity(),
                             -std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()};
    for (const double value : values)
    {
        const auto x = Opaque(value);
        const auto f = Opaque(static_cast<float>(value));
        REQUIRE(SameBits(fstd::fabs(x), std::fabs(x)));
        REQUIRE(SameBits(fstd::floor(x), std::floor(x)));
        REQUIRE(SameBits(fstd::ceil(f), std::ceil(f)));
        REQUIRE(SameBits(fstd::trunc(x), std::trunc(x)));
        REQUIRE(SameBits(fstd::round(f), std::round(f)));
        REQUIRE(SameBits(fstd::rint(x), std::rint(x)));
        REQUIRE(SameBits(fstd::nearbyint(f), std::nearbyint(f)));
        REQUIRE(SameBits(fstd::sqrt(x), std::sqrt(x)));
        REQUIRE(SameBits(fstd::exp(x), std::exp(x)));
        REQUIRE(SameBits(fstd::log2(f), std::log2(f)));
        REQUIRE(SameBits(fstd::tanh(x), std::tanh(x)));
        REQUIRE(SameBits(fstd::fmin(x, 1.0), std::fmin(x, 1.0)));
        REQUIRE(SameBits(fstd::remainder(f, 0.75f), std::remainder(f, 0.75f)));
        int ours = 0;
        int host = 0;
        REQUIRE(SameBits(fstd::frexp(f, &ours), std::frexp(f, &host)));
        if (std::isfinite(value))
        {
            REQUIRE(ours == host);
        }
        REQUIRE(SameBits(fstd::ldexp(f, 3), std::ldexp(f, 3)));
        float whole = 0;
        float hostWhole = 0;
        REQUIRE(SameBits(fstd::modf(f, &whole), std::modf(f, &hostWhole)));
        REQUIRE(SameBits(whole, hostWhole));

        REQUIRE(fstd::fpclassify(x) == std::fpclassify(x));
        REQUIRE(fstd::fpclassify(f) == std::fpclassify(f));
        REQUIRE(fstd::isfinite(x) == std::isfinite(x));
        REQUIRE(fstd::isinf(f) == std::isinf(f));
        REQUIRE(fstd::isnan(x) == std::isnan(x));
        REQUIRE(fstd::isnormal(f) == std::isnormal(f));
        REQUIRE(fstd::signbit(x) == std::signbit(x));
        REQUIRE(fstd::isless(x, 1) == std::isless(x, 1.0));
        REQUIRE(fstd::isunordered(f, 0.0) == std::isunordered(f, 0.0));
        REQUIRE(fstd::islessgreater(x, 0.5f) == std::islessgreater(x, 0.5));
    }
    REQUIRE(fstd::fpclassify(0) == FP_ZERO);
    REQUIRE((fstd::isnormal(3) && !fstd::isnan(3) && fstd::signbit(-3)));
}

TEST_CASE("cmath's hypot and lerp", "[libcxx]")
{
    REQUIRE(fstd::hypot(Opaque(3.0), 4.0) == 5.0);
    REQUIRE(fstd::hypot(Opaque(2.0), 3.0, 6.0) == 7.0);
    REQUIRE(fstd::hypot(Opaque(1e300), 1e300, 1e300) > 1e300);
    REQUIRE(fstd::hypot(Opaque(0.0), -0.0, 0.0) == 0.0);
    REQUIRE(fstd::isinf(fstd::hypot(Opaque(1.0), std::numeric_limits<double>::infinity(), 2.0)));

    STATIC_REQUIRE(fstd::lerp(2.0, 4.0, 0.5) == 3.0);
    STATIC_REQUIRE(fstd::lerp(-1.0, 1.0, 0.25) == -0.5);
    REQUIRE(fstd::lerp(Opaque(1.0), 3.0, 1.0) == 3.0);
    REQUIRE(fstd::lerp(Opaque(1.0), 3.0, 2.0) == 5.0);
    REQUIRE(fstd::lerp(Opaque(0.1), 0.1, 0.7) == 0.1);
    REQUIRE(fstd::lerp(Opaque(5.0f), 1.0f, 0.5f) == 3.0f);
    double previous = fstd::lerp(0.1, 0.7, 0.0);
    for (int step = 1; step <= 1000; ++step)
    {
        const double next = fstd::lerp(0.1, 0.7, Opaque(step / 1000.0));
        REQUIRE(next >= previous);
        previous = next;
    }
    REQUIRE(previous == 0.7);
}

TEST_CASE("exception has a message and a terminate handler", "[libcxx]")
{
    const fstd::exception base;
    const fstd::bad_exception bad;
    const Derived derived;
    const fstd::exception& reference = derived;
    REQUIRE(fstd::string_view(base.what()) == "std::exception");
    REQUIRE(fstd::string_view(bad.what()) == "std::bad_exception");
    REQUIRE(fstd::string_view(reference.what()) == "derived");
    REQUIRE(fstd::uncaught_exceptions() == 0);

    const fstd::terminate_handler before = fstd::set_terminate(TerminateHandler);
    REQUIRE(fstd::get_terminate() == &TerminateHandler);
    REQUIRE(fstd::set_terminate(before) == &TerminateHandler);
    REQUIRE(fstd::get_terminate() == before);
}
