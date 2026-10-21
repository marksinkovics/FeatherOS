#include <catch2/catch_test_macros.hpp>

#include "libc/scan.h"

#include <climits>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{

struct TextSource
{
    const char* text;

    int Get()
    {
        return *text == '\0' ? -1 : static_cast<unsigned char>(*text++);
    }

    void Unget(int)
    {
        --text;
    }
};

int Scan(const char* text, const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    TextSource source = {text};
    const int count = FeatherOS::LibC::ScanFrom(source, format, arguments);
    va_end(arguments);
    return count;
}

// Scans one integer with both engines and requires the same count and value.
template <typename Value>
void RequireSameInteger(const char* text, const char* format)
{
    Value ours = 0;
    Value host = 0;
    INFO("\"" << text << "\" with " << format);
    REQUIRE(Scan(text, format, &ours) == std::sscanf(text, format, &host));
    REQUIRE(ours == host);
}

} // namespace

TEST_CASE("scanf reads integers in each base the way the host does")
{
    for (const char* text : {"42", "  -17", "+8", "0", "0x1f", "0X1F", "017", "12abc", "-0",
                             "2147483647", "-2147483648", "  \t\n99"})
    {
        RequireSameInteger<int>(text, "%d");
        RequireSameInteger<int>(text, "%i");
        RequireSameInteger<unsigned>(text, "%u");
        RequireSameInteger<unsigned>(text, "%x");
        RequireSameInteger<unsigned>(text, "%o");
    }
    RequireSameInteger<int>("12345", "%3d");
    RequireSameInteger<long long>("-9223372036854775808", "%lld");
    RequireSameInteger<unsigned long long>("18446744073709551615", "%llu");
    RequireSameInteger<short>("-1234", "%hd");
    RequireSameInteger<signed char>("-12", "%hhd");
    RequireSameInteger<size_t>("4096", "%zu");
}

TEST_CASE("scanf matches literal text and white space")
{
    int hours = 0;
    int minutes = 0;
    REQUIRE(Scan("12:34", "%d:%d", &hours, &minutes) == 2);
    REQUIRE(hours == 12);
    REQUIRE(minutes == 34);
    REQUIRE(Scan("12-34", "%d:%d", &hours, &minutes) == 1);
    REQUIRE(Scan("x=  5 , y=6", "x=%d , y=%d", &hours, &minutes) == 2);
    REQUIRE(minutes == 6);
    REQUIRE(Scan("100%", "%d%%", &hours) == 1);
    REQUIRE(hours == 100);
}

TEST_CASE("scanf reads strings, characters, and scansets")
{
    char word[16];
    char rest[16];
    REQUIRE(Scan("  feather os", "%s %s", word, rest) == 2);
    REQUIRE(std::string(word) == "feather");
    REQUIRE(std::string(rest) == "os");
    REQUIRE(Scan("featheros", "%4s%s", word, rest) == 2);
    REQUIRE(std::string(word) == "feat");
    REQUIRE(std::string(rest) == "heros");

    char letters[3] = {};
    REQUIRE(Scan(" ab", "%c%c", &letters[0], &letters[1]) == 2);
    REQUIRE(letters[0] == ' ');
    REQUIRE(letters[1] == 'a');
    char pair[2];
    REQUIRE(Scan("xyz", "%2c", pair) == 1);
    REQUIRE(std::memcmp(pair, "xy", 2) == 0);

    REQUIRE(Scan("key=value;", "%[a-z]=%[^;]", word, rest) == 2);
    REQUIRE(std::string(word) == "key");
    REQUIRE(std::string(rest) == "value");
    REQUIRE(Scan("]]x", "%[]]", word) == 1);
    REQUIRE(std::string(word) == "]]");
    REQUIRE(Scan("abc", "%[0-9]", word) == 0);
}

TEST_CASE("scanf skips suppressed fields and counts consumed characters")
{
    int value = 0;
    int consumed = 0;
    REQUIRE(Scan("10 20 30", "%*d %*d %d%n", &value, &consumed) == 1);
    REQUIRE(value == 30);
    REQUIRE(consumed == 8);
    char word[8];
    REQUIRE(Scan("skip keep", "%*s %s", word) == 1);
    REQUIRE(std::string(word) == "keep");
}

TEST_CASE("scanf returns -1 only when input ends before the first conversion")
{
    int value = 0;
    REQUIRE(Scan("", "%d", &value) == -1);
    REQUIRE(Scan("   ", "%d", &value) == -1);
    REQUIRE(Scan("abc", "%d", &value) == 0);
    REQUIRE(Scan("5", "%d %d", &value, &value) == 1);
    REQUIRE(Scan("", "literal") == -1);
}

TEST_CASE("scanf stops at floating-point conversions for now")
{
    int value = 0;
    REQUIRE(Scan("1.5 2", "%f %d", &value) == 0);
    void* pointer = nullptr;
    REQUIRE(Scan("0x1234", "%p", &pointer) == 1);
    REQUIRE(pointer == reinterpret_cast<void*>(0x1234));
}
