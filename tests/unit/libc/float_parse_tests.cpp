#include <catch2/catch_test_macros.hpp>

#include "libc/float_parse.h"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{

using FeatherOS::LibC::ParsedFloat;

template <typename Value>
Value ToValue(const ParsedFloat& parsed)
{
    return FeatherOS::LibC::FromRawBits<Value>(parsed.bits);
}

// Requires the same bits, end, and range error as the host's strtod.
void RequireSameDouble(const char* text)
{
    INFO(text);
    const ParsedFloat ours = FeatherOS::LibC::ParseFloat(FeatherOS::LibC::DoubleFormat, text);
    errno = 0;
    char* host_end = nullptr;
    const double host = std::strtod(text, &host_end);
    const bool host_range = errno == ERANGE;
    uint64_t host_bits;
    std::memcpy(&host_bits, &host, sizeof(host_bits));
    if (std::isnan(host))
    {
        REQUIRE(std::isnan(ToValue<double>(ours)));
    }
    else
    {
        REQUIRE(static_cast<uint64_t>(ours.bits) == host_bits);
    }
    REQUIRE(ours.end == host_end);
    // macOS reports ERANGE for every subnormal result, exact or not, so only check the rest.
    if (std::fabs(host) >= 2.2250738585072014e-308 || host == 0.0)
    {
        REQUIRE(ours.range_error == host_range);
    }
}

void RequireSameFloat(const char* text)
{
    INFO(text);
    const ParsedFloat ours = FeatherOS::LibC::ParseFloat(FeatherOS::LibC::FloatFormat, text);
    const float host = std::strtof(text, nullptr);
    uint32_t host_bits;
    std::memcpy(&host_bits, &host, sizeof(host_bits));
    REQUIRE(static_cast<uint32_t>(ours.bits) == host_bits);
}

} // namespace

TEST_CASE("strtod rounds decimal text exactly like the host")
{
    for (const char* text :
         {"0", "-0", "1", "0.1", "0.5", "1.5", "  +3.25", "123.456", "1e10", "1E-10", "1e308",
          "1.7976931348623157e308", "1.7976931348623158e308", "2.2250738585072014e-308",
          "2.2250738585072011e-308", "4.9406564584124654e-324", "2.4703282292062327e-324",
          "2.4703282292062328e-324", "1e-400", "1e400", "9007199254740993", "9007199254740992.5",
          "0.000000000000000000000000000000000001", "3.14159265358979323846264338327950288",
          "123456789012345678901234567890e-20", "1.00000000000000011102230246251565404",
          "1.000000000000000111022302462515654042363166809082031250000000001", ".5", "5.",
          "1e", "1e+", "1.5e+x", "abc", "-", ".", "0x", "12abc", "7.0e-1", "100000000000000000000",
          "6.103515625e-05", "8.98846567431158e307"})
    {
        RequireSameDouble(text);
    }
}

TEST_CASE("strtod reads hexadecimal floats, infinity, and NaN")
{
    for (const char* text : {"0x1p0", "0x1.8p1", "-0X1.FFFFFFFFFFFFFP1023", "0x1p-1074",
                             "0x1p-1075", "0x1.0000000000000801p0", "0x.8p1", "0x10", "0x1p",
                             "0x1.8", "inf", "-INFINITY", "infinite", "nan", "NaN(123)", "nan(",
                             "0xg"})
    {
        RequireSameDouble(text);
    }
}

TEST_CASE("strtof rounds to single precision directly")
{
    for (const char* text : {"0.1", "1.17549435e-38", "1e-45", "3.4028235e38", "3.4028236e38",
                             "16777217", "16777219", "1.000000059604644775390625",
                             "1.0000000596046447753906251", "0x1.fffffep127", "1e-50"})
    {
        RequireSameFloat(text);
    }
}

TEST_CASE("digits past the 800 kept still break ties correctly")
{
    // 1 + 2^-53 is exactly halfway between two doubles: the tie goes to the even one, unless a
    // later digit makes the value larger.
    const std::string half = "1.00000000000000011102230246251565404236316680908203125";
    std::string above = half + std::string(900, '0') + "1";
    RequireSameDouble(half.c_str());
    RequireSameDouble(above.c_str());
    std::string long_integer = "9007199254740993" + std::string(850, '0');
    RequireSameDouble(long_integer.c_str());
}
