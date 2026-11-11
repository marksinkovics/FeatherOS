#include <catch2/catch_test_macros.hpp>

#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <string>

#include "libcxx/include/charconv"

namespace
{

namespace fstd = featheros_std;

constexpr bool IntegersAtCompileTime()
{
    char buffer[16] = {};
    const auto written = fstd::to_chars(buffer, buffer + 16, -1234, 10);
    int value = 0;
    const auto read = fstd::from_chars(buffer, written.ptr, value);
    return written.ec == fstd::errc{} && value == -1234 && read.ptr == written.ptr;
}
static_assert(IntegersAtCompileTime());

template <class T> void CheckIntegerToChars(T value, int base)
{
    char ours[80];
    char theirs[80];
    const auto our = fstd::to_chars(ours, ours + sizeof(ours), value, base);
    const auto their = std::to_chars(theirs, theirs + sizeof(theirs), value, base);
    REQUIRE(our.ec == fstd::errc{});
    REQUIRE(std::string(ours, our.ptr) == std::string(theirs, their.ptr));
    T read = 0;
    const auto back = fstd::from_chars(ours, our.ptr, read, base);
    REQUIRE(back.ec == fstd::errc{});
    REQUIRE(back.ptr == our.ptr);
    REQUIRE(read == value);
}

template <class T> void IntegerRoundTrips(std::mt19937_64& generator)
{
    for (int base = 2; base <= 36; ++base)
    {
        CheckIntegerToChars<T>(0, base);
        CheckIntegerToChars<T>(std::numeric_limits<T>::min(), base);
        CheckIntegerToChars<T>(std::numeric_limits<T>::max(), base);
        for (int round = 0; round < 40; ++round)
        {
            CheckIntegerToChars(static_cast<T>(generator()), base);
        }
    }
}

std::string Shortest(double value)
{
    char buffer[400];
    const auto result = fstd::to_chars(buffer, buffer + sizeof(buffer), value);
    REQUIRE(result.ec == fstd::errc{});
    return std::string(buffer, result.ptr);
}

template <class T> std::string Ours(T value, int format, int precision)
{
    char buffer[2000];
    fstd::to_chars_result result{};
    if (format < 0)
    {
        result = fstd::to_chars(buffer, buffer + sizeof(buffer), value);
    }
    else if (precision < 0)
    {
        result = fstd::to_chars(buffer, buffer + sizeof(buffer), value,
                                static_cast<fstd::chars_format>(format));
    }
    else
    {
        result = fstd::to_chars(buffer, buffer + sizeof(buffer), value,
                                static_cast<fstd::chars_format>(format), precision);
    }
    REQUIRE(result.ec == fstd::errc{});
    return std::string(buffer, result.ptr);
}

template <class T> std::string Theirs(T value, int format, int precision)
{
    char buffer[2000];
    std::to_chars_result result{};
    if (format < 0)
    {
        result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    }
    else if (precision < 0)
    {
        result = std::to_chars(buffer, buffer + sizeof(buffer), value,
                               static_cast<std::chars_format>(format));
    }
    else
    {
        result = std::to_chars(buffer, buffer + sizeof(buffer), value,
                               static_cast<std::chars_format>(format), precision);
    }
    return std::string(buffer, result.ptr);
}

// Hex text differs between libraries for subnormals, so it is checked by reading it back.
template <class T> void CheckFloat(T value)
{
    for (int format : {-1, 1, 2, 3})
    {
        INFO("format " << format << " value " << Theirs(value, -1, -1));
        REQUIRE(Ours(value, format, -1) == Theirs(value, format, -1));
    }
    for (int precision : {0, 1, 3, 6, 17, 40})
    {
        for (int format : {1, 2, 3})
        {
            INFO("format " << format << " precision " << precision << " value "
                           << Theirs(value, -1, -1));
            REQUIRE(Ours(value, format, precision) == Theirs(value, format, precision));
        }
    }
    const std::string hex = Ours(value, 4, -1);
    T back{};
    const auto read = std::from_chars(hex.data(), hex.data() + hex.size(), back,
                                      std::chars_format::hex);
    REQUIRE(read.ptr == hex.data() + hex.size());
    if (std::isnan(value))
    {
        REQUIRE(std::isnan(back));
    }
    else
    {
        REQUIRE(std::memcmp(&back, &value, sizeof(T)) == 0);
    }
}

template <class T> void CheckFromChars(const std::string& text, int format)
{
    T ours = 7;
    T theirs = 7;
    const char* first = text.data();
    const char* last = first + text.size();
    const auto our = fstd::from_chars(first, last, ours, static_cast<fstd::chars_format>(format));
    const auto their = std::from_chars(first, last, theirs, static_cast<std::chars_format>(format));
    INFO("text '" << text << "' format " << format);
    REQUIRE(our.ptr == their.ptr);
    // Both errors have the same numbers on macOS and Linux.
    REQUIRE(static_cast<int>(our.ec) == static_cast<int>(their.ec));
    // The standard leaves the value alone when it is out of range; libc++ stores infinity or 0.
    if (our.ec == fstd::errc::result_out_of_range)
    {
        REQUIRE(ours == 7);
        return;
    }
    if (std::isnan(theirs))
    {
        REQUIRE(std::isnan(ours));
        REQUIRE(std::signbit(ours) == std::signbit(theirs));
    }
    else
    {
        REQUIRE(std::memcmp(&ours, &theirs, sizeof(T)) == 0);
    }
}

} // namespace

TEST_CASE("charconv integers in every base", "[libcxx][charconv]")
{
    std::mt19937_64 generator(31);
    IntegerRoundTrips<char>(generator);
    IntegerRoundTrips<signed char>(generator);
    IntegerRoundTrips<unsigned char>(generator);
    IntegerRoundTrips<short>(generator);
    IntegerRoundTrips<unsigned short>(generator);
    IntegerRoundTrips<int>(generator);
    IntegerRoundTrips<unsigned>(generator);
    IntegerRoundTrips<long>(generator);
    IntegerRoundTrips<unsigned long>(generator);
    IntegerRoundTrips<long long>(generator);
    IntegerRoundTrips<unsigned long long>(generator);
}

TEST_CASE("charconv integer errors", "[libcxx][charconv]")
{
    char small[3];
    auto written = fstd::to_chars(small, small + 3, 1234);
    REQUIRE(written.ec == fstd::errc::value_too_large);
    REQUIRE(written.ptr == small + 3);
    written = fstd::to_chars(small, small + 3, -12);
    REQUIRE(written.ec == fstd::errc{});
    REQUIRE(std::string(small, written.ptr) == "-12");

    int value = 99;
    const std::string inputs[] = {"", "-", "x12", "2147483648", "-2147483649", "-2147483648",
                                  "12abc", "+5", " 5", "0x10", "00017"};
    for (const std::string& text : inputs)
    {
        int ours = 99;
        int theirs = 99;
        const auto our = fstd::from_chars(text.data(), text.data() + text.size(), ours);
        const auto their = std::from_chars(text.data(), text.data() + text.size(), theirs);
        INFO("text '" << text << "'");
        REQUIRE(our.ptr == their.ptr);
        REQUIRE(static_cast<int>(our.ec) == static_cast<int>(their.ec));
        REQUIRE(ours == theirs);
    }
    unsigned unsigned_value = 5;
    const char negative[] = "-1";
    REQUIRE(fstd::from_chars(negative, negative + 2, unsigned_value).ec ==
            fstd::errc::invalid_argument);
    const char base36[] = "zz";
    REQUIRE(fstd::from_chars(base36, base36 + 2, value, 36).ec == fstd::errc{});
    REQUIRE(value == 35 * 36 + 35);
    STATIC_REQUIRE(static_cast<int>(fstd::errc::invalid_argument) == 22);
    STATIC_REQUIRE((fstd::chars_format::fixed | fstd::chars_format::scientific) ==
                   fstd::chars_format::general);
}

TEST_CASE("charconv shortest floating-point text", "[libcxx][charconv]")
{
    REQUIRE(Shortest(0.1) == "0.1");
    REQUIRE(Shortest(1e23) == "1e+23");
    REQUIRE(Shortest(-0.0) == "-0");
    REQUIRE(Shortest(123456.0) == "123456");
    REQUIRE(Shortest(std::numeric_limits<double>::infinity()) == "inf");
    REQUIRE(Shortest(-std::numeric_limits<double>::quiet_NaN()) == "-nan");
    REQUIRE(Ours(1e23, 2, -1) == "99999999999999991611392");
    REQUIRE(Ours(1152921504606846976.0, -1, -1) == "1152921504606846976");
    REQUIRE(Ours(1e21, 2, -1) == "1000000000000000000000");

    const double doubles[] = {0.0, -0.0, 1.0, 0.1, 0.3, 2.0 / 3, 1e-7, 1e21, 1e22, 1e23,
                              5e-324, 2.2250738585072014e-308, 1.7976931348623157e308,
                              9007199254740993.0, 4.35, 123456789012345680.0, 1e100, 3e-5};
    for (double value : doubles)
    {
        CheckFloat(value);
        CheckFloat(-value);
    }
    const float floats[] = {0.0F, 1.0F, 0.1F, 3.4028235e38F, 1e-45F, 1.17549435e-38F, 16777217.0F,
                            7e-3F, 1e10F};
    for (float value : floats)
    {
        CheckFloat(value);
    }
    CheckFloat(std::numeric_limits<double>::infinity());
    CheckFloat(-std::numeric_limits<float>::infinity());

    std::mt19937_64 generator(41);
    for (int round = 0; round < 1500; ++round)
    {
        const std::uint64_t bits = generator();
        double value = 0;
        std::memcpy(&value, &bits, sizeof(value));
        if (!std::isnan(value))
        {
            CheckFloat(value);
        }
        const auto float_bits = static_cast<std::uint32_t>(bits >> 7);
        float single = 0;
        std::memcpy(&single, &float_bits, sizeof(single));
        if (!std::isnan(single))
        {
            CheckFloat(single);
        }
    }
    // Values a few units in the last place from powers of ten and two, where shortest digits are
    // hardest to find.
    for (int exponent = -320; exponent <= 308; exponent += 7)
    {
        double value = std::pow(10.0, exponent);
        for (int step = 0; step < 3; ++step)
        {
            if (value != 0 && std::isfinite(value))
            {
                CheckFloat(value);
            }
            value = std::nextafter(value, 2 * value + 1);
        }
    }
    for (int exponent = -1070; exponent <= 1023; exponent += 13)
    {
        CheckFloat(std::ldexp(1.0, exponent));
        CheckFloat(std::nextafter(std::ldexp(1.0, exponent), 0.0));
    }
}

TEST_CASE("charconv floating-point output that does not fit", "[libcxx][charconv]")
{
    char buffer[4];
    auto result = fstd::to_chars(buffer, buffer + 4, 12345.0);
    REQUIRE(result.ec == fstd::errc::value_too_large);
    REQUIRE(result.ptr == buffer + 4);
    result = fstd::to_chars(buffer, buffer + 4, 1.5, fstd::chars_format::fixed, 3);
    REQUIRE(result.ec == fstd::errc::value_too_large);
    result = fstd::to_chars(buffer, buffer + 4, 1.5);
    REQUIRE(result.ec == fstd::errc{});
    REQUIRE(std::string(buffer, result.ptr) == "1.5");
}

TEST_CASE("charconv floating-point input", "[libcxx][charconv]")
{
    const std::string texts[] = {
        "1",          "-1.5",         "0.1",       "1e10",        "1E-10",      "1e",
        "1e+",        "1.5e-x",       ".5",        "5.",          ".",          "-",
        "+1",         "inf",          "-INFINITY", "infinit",     "nan",        "-nan(abc_1)",
        "nan(",       "1e400",        "-1e400",    "1e-400",      "4.9e-324",   "2.4e-324",
        "2.5e-324",   "0x1p3",        "1p3",       "1.8p+1",      "abc",        "0.000001234",
        "123456789012345678901234567890", "1.7976931348623158e308", "1.7976931348623159e308",
        "00000000000000000000000000001.5", "0.0e5", "-0", "1.0000000000000002", "9007199254740993",
        "fF.8P-2",    "1e-2147483649", "1e2147483648"};
    for (const std::string& text : texts)
    {
        for (int format : {1, 2, 3, 4})
        {
            CheckFromChars<double>(text, format);
            CheckFromChars<float>(text, format);
        }
    }

    // A tie that only a digit far past the 800 kept ones breaks.
    std::string tie = "9007199254740993";
    tie += ".";
    tie += std::string(2000, '0');
    CheckFromChars<double>(tie, 3);
    tie += "1";
    CheckFromChars<double>(tie, 3);
    CheckFromChars<double>("0." + std::string(3000, '0') + "1e3000", 3);

    std::mt19937_64 generator(43);
    for (int round = 0; round < 2000; ++round)
    {
        const std::uint64_t bits = generator();
        double value = 0;
        std::memcpy(&value, &bits, sizeof(value));
        if (std::isnan(value))
        {
            continue;
        }
        char text[64];
        std::snprintf(text, sizeof(text), round % 2 == 0 ? "%.17g" : "%.5e", value);
        CheckFromChars<double>(text, 3);
        CheckFromChars<float>(text, 3);
        const std::string shortest = Shortest(value);
        double back = 0;
        const auto read = fstd::from_chars(shortest.data(), shortest.data() + shortest.size(), back);
        REQUIRE(read.ec == fstd::errc{});
        REQUIRE(std::memcmp(&back, &value, sizeof(value)) == 0);
    }
}
