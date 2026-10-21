#include <catch2/catch_test_macros.hpp>

#include "libc/math_functions.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace
{

namespace Math = FeatherOS::LibC::Math;

// Doubles ordered as integers, so the difference counts representable values between them.
int64_t Ordered(double value)
{
    int64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits < 0 ? std::numeric_limits<int64_t>::min() - bits : bits;
}

uint64_t UlpDistance(double ours, double host)
{
    if (std::isnan(ours) && std::isnan(host))
    {
        return 0;
    }
    if (std::isnan(ours) || std::isnan(host))
    {
        return UINT64_MAX;
    }
    const int64_t difference = Ordered(ours) - Ordered(host);
    return static_cast<uint64_t>(difference < 0 ? -difference : difference);
}

bool SameBits(double ours, double host)
{
    return (std::isnan(ours) && std::isnan(host)) ||
           std::memcmp(&ours, &host, sizeof(double)) == 0;
}

// Deterministic values spread over [low, high].
class Sweep
{
public:
    Sweep(double low_value, double high_value) : low(low_value), high(high_value) {}

    double Next()
    {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        const double unit = static_cast<double>(state >> 11) * 0x1p-53;
        return low + (high - low) * unit;
    }

private:
    double low;
    double high;
    uint64_t state = 12345;
};

template <typename Ours, typename Host>
void RequireWithin(uint64_t ulps, double low, double high, Ours ours, Host host)
{
    Sweep sweep(low, high);
    uint64_t worst = 0;
    double worst_input = 0;
    for (int index = 0; index < 20000; ++index)
    {
        const double value = sweep.Next();
        const uint64_t distance = UlpDistance(ours(value), host(value));
        if (distance > worst)
        {
            worst = distance;
            worst_input = value;
        }
    }
    INFO("worst input " << worst_input << " is " << worst << " ulps off");
    REQUIRE(worst <= ulps);
}

} // namespace

TEST_CASE("exact functions match the host bit for bit")
{
    const double values[] = {0.0,   -0.0,  0.5,   -0.5, 1.5,     -1.5,   2.5,    -2.5,
                             3.7,   -3.7,  1e300, -1e-300, 4.9e-324, 1e16, 0x1p52 + 0.5,
                             123.0, std::numeric_limits<double>::infinity(),
                             -std::numeric_limits<double>::infinity(), std::nan("")};
    for (const double value : values)
    {
        INFO(value);
        REQUIRE(SameBits(Math::Floor(value), std::floor(value)));
        REQUIRE(SameBits(Math::Ceil(value), std::ceil(value)));
        REQUIRE(SameBits(Math::Trunc(value), std::trunc(value)));
        REQUIRE(SameBits(Math::Round(value), std::round(value)));
        REQUIRE(SameBits(Math::Abs(value), std::fabs(value)));
        REQUIRE(SameBits(Math::Sqrt(Math::Abs(value)), std::sqrt(std::fabs(value))));
        int ours_exponent = 0;
        int host_exponent = 0;
        REQUIRE(SameBits(Math::SplitExponent(value, &ours_exponent),
                         std::frexp(value, &host_exponent)));
        if (std::isfinite(value))
        {
            REQUIRE(ours_exponent == host_exponent);
        }
        double ours_integer = 0;
        double host_integer = 0;
        REQUIRE(SameBits(Math::SplitInteger(value, &ours_integer), std::modf(value, &host_integer)));
        REQUIRE(SameBits(ours_integer, host_integer));
        for (const int exponent : {-1100, -1074, -1030, -10, 0, 7, 1000, 2000})
        {
            REQUIRE(SameBits(Math::ScaleB(value, exponent), std::ldexp(value, exponent)));
        }
        for (const double divisor : {1.0, -0.3, 7.0, 1e-300, 0x1p-1060, 1e300, 0.0})
        {
            REQUIRE(SameBits(Math::Remainder(value, divisor), std::fmod(value, divisor)));
        }
    }
}

TEST_CASE("exp, exp2, and the logarithms stay within 2 ulps")
{
    RequireWithin(1, -745, 709.7, Math::Exp, [](double x) { return std::exp(x); });
    RequireWithin(1, -5, 5, Math::Exp, [](double x) { return std::exp(x); });
    RequireWithin(1, -1074, 1023.9, Math::Exp2, [](double x) { return std::exp2(x); });
    RequireWithin(1, 1e-300, 1e300, Math::Log, [](double x) { return std::log(x); });
    RequireWithin(2, 0.5, 2, Math::Log, [](double x) { return std::log(x); });
    RequireWithin(2, 1e-300, 1e300, Math::Log2, [](double x) { return std::log2(x); });
    RequireWithin(2, 0.5, 2, Math::Log10, [](double x) { return std::log10(x); });
    RequireWithin(2, 1e-300, 1e300, Math::Log10, [](double x) { return std::log10(x); });
    REQUIRE(Math::Log2(1024.0) == 10.0);
    REQUIRE(Math::Exp(0.0) == 1.0);
    REQUIRE(Math::Log(1.0) == 0.0);
    REQUIRE(std::isinf(Math::Exp(710)));
    REQUIRE(Math::Exp(-746) == 0.0);
    REQUIRE(std::isnan(Math::Log(-1)));
    REQUIRE(Math::Log(0.0) == -std::numeric_limits<double>::infinity());
}

TEST_CASE("pow stays within 2 ulps, even for large results")
{
    Sweep bases(1e-3, 1e3);
    Sweep exponents(-100, 100);
    uint64_t worst = 0;
    for (int index = 0; index < 20000; ++index)
    {
        const double x = bases.Next();
        const double y = exponents.Next();
        const uint64_t distance = UlpDistance(Math::Pow(x, y), std::pow(x, y));
        worst = distance > worst ? distance : worst;
    }
    REQUIRE(worst <= 2);
    REQUIRE(Math::Pow(2, 10) == 1024);
    REQUIRE(Math::Pow(10, 22) == 1e22);
    REQUIRE(Math::Pow(-2, 3) == -8);
}

TEST_CASE("pow follows every special case of the C standard")
{
    const double infinity = std::numeric_limits<double>::infinity();
    const double values[] = {0.0, -0.0, 1.0, -1.0, 2.0, -2.0, 0.5, -0.5, 3.0, -3.0, 2.5, -2.5,
                             infinity, -infinity, std::nan("")};
    for (const double x : values)
    {
        for (const double y : values)
        {
            INFO("pow(" << x << ", " << y << ")");
            const double host = std::pow(x, y);
            // Special cases are exact; ordinary results like pow(2, -0.5) are only close.
            if (std::isfinite(host) && host != 0 && std::fabs(host) != 1)
            {
                REQUIRE(UlpDistance(Math::Pow(x, y), host) <= 2);
            }
            else
            {
                REQUIRE(SameBits(Math::Pow(x, y), host));
            }
        }
    }
}

TEST_CASE("sin, cos, and tan stay within a few ulps below 10^5")
{
    RequireWithin(2, -3.2, 3.2, Math::Sin, [](double x) { return std::sin(x); });
    RequireWithin(2, -3.2, 3.2, Math::Cos, [](double x) { return std::cos(x); });
    RequireWithin(2, -1e5, 1e5, Math::Sin, [](double x) { return std::sin(x); });
    RequireWithin(2, -1e5, 1e5, Math::Cos, [](double x) { return std::cos(x); });
    RequireWithin(3, -1.5, 1.5, Math::Tan, [](double x) { return std::tan(x); });
    REQUIRE(Math::Sin(0.0) == 0.0);
    REQUIRE(std::signbit(Math::Sin(-0.0)));
    REQUIRE(Math::Cos(0.0) == 1.0);
    REQUIRE(std::isnan(Math::Sin(std::numeric_limits<double>::infinity())));
}

TEST_CASE("the inverse and hyperbolic functions stay within a few ulps")
{
    RequireWithin(2, -10, 10, Math::ArcTan, [](double x) { return std::atan(x); });
    RequireWithin(2, -1e300, 1e300, Math::ArcTan, [](double x) { return std::atan(x); });
    RequireWithin(3, -1, 1, Math::ArcSin, [](double x) { return std::asin(x); });
    RequireWithin(3, -1, 1, Math::ArcCos, [](double x) { return std::acos(x); });
    RequireWithin(3, -20, 20, Math::Sinh, [](double x) { return std::sinh(x); });
    RequireWithin(3, -710, 710, Math::Cosh, [](double x) { return std::cosh(x); });
    RequireWithin(3, -20, 20, Math::Tanh, [](double x) { return std::tanh(x); });
    Sweep ys(-100, 100);
    Sweep xs(-100, 100);
    uint64_t worst = 0;
    for (int index = 0; index < 20000; ++index)
    {
        const double y = ys.Next();
        const double x = xs.Next();
        const uint64_t distance = UlpDistance(Math::ArcTan2(y, x), std::atan2(y, x));
        const uint64_t hypot_distance = UlpDistance(Math::Hypot(x, y), std::hypot(x, y));
        worst = distance > worst ? distance : worst;
        worst = hypot_distance > worst ? hypot_distance : worst;
    }
    REQUIRE(worst <= 3);
}

TEST_CASE("atan2 follows the special cases of the C standard")
{
    const double infinity = std::numeric_limits<double>::infinity();
    const double values[] = {0.0, -0.0, 1.0, -1.0, infinity, -infinity};
    for (const double y : values)
    {
        for (const double x : values)
        {
            INFO("atan2(" << y << ", " << x << ")");
            REQUIRE(SameBits(Math::ArcTan2(y, x), std::atan2(y, x)));
        }
    }
}
