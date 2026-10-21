#include <catch2/catch_test_macros.hpp>

#include "libc/format.h"

#include <climits>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

namespace
{

struct StringSink
{
    std::string text;

    void Put(char character)
    {
        text.push_back(character);
    }
};

std::string Format(const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    StringSink sink;
    const int count = FeatherOS::LibC::FormatTo(sink, format, arguments);
    va_end(arguments);
    REQUIRE(count == static_cast<int>(sink.text.size()));
    return sink.text;
}

// The host C library is the reference for everything both support.
std::string Host(const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    char buffer[256];
    std::vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    return buffer;
}

#define REQUIRE_SAME(...) REQUIRE(Format(__VA_ARGS__) == Host(__VA_ARGS__))

int Truncated(char* buffer, size_t size, const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    FeatherOS::LibC::BufferSink sink = {buffer, size};
    const int count = FeatherOS::LibC::FormatTo(sink, format, arguments);
    sink.Terminate();
    va_end(arguments);
    return count;
}

} // namespace

TEST_CASE("printf formats integers with every flag the way the host does")
{
    const char* formats[] = {
        "%d",   "%5d",   "%-5d|", "%05d",  "%+d",   "% d",   "%.3d",  "%8.3d", "%-8.3d|",
        "%08.3d", "%+05d", "%i",  "%u",    "%x",    "%X",    "%#x",   "%#X",   "%o",
        "%#o",  "%#.0o", "%.0d",  "%.0x",  "%#08x", "%-#8x|", "%+.0d", "% 06d", "%-+6d|",
    };
    const int values[] = {0, 1, -1, 7, 42, -42, 255, -255, INT_MAX, INT_MIN};
    for (const char* format : formats)
    {
        for (const int value : values)
        {
            INFO(format << " with " << value);
            REQUIRE_SAME(format, value);
        }
    }
}

TEST_CASE("printf reads each length modifier's argument type")
{
    REQUIRE_SAME("%ld %ld", LONG_MIN, LONG_MAX);
    REQUIRE_SAME("%lld %llu", LLONG_MIN, ULLONG_MAX);
    REQUIRE_SAME("%lu %lx %lo", ULONG_MAX, ULONG_MAX, ULONG_MAX);
    REQUIRE_SAME("%hhd %hhu %hd %hu", 300, 300, 70000, 70000);
    REQUIRE_SAME("%zu %zd", static_cast<size_t>(123456789012), static_cast<ptrdiff_t>(-5));
    REQUIRE_SAME("%jd %ju", static_cast<intmax_t>(INT64_MIN), static_cast<uintmax_t>(UINT64_MAX));
    REQUIRE_SAME("%td", static_cast<ptrdiff_t>(-99));
}

TEST_CASE("printf formats characters and strings with width and precision")
{
    REQUIRE_SAME("[%s]", "feather");
    REQUIRE_SAME("[%10s] [%-10s] [%.3s] [%10.3s] [%-10.3s]", "feather", "feather", "feather",
                 "feather", "feather");
    REQUIRE_SAME("[%.0s] [%.20s]", "feather", "os");
    REQUIRE_SAME("[%c] [%3c] [%-3c]", 'a', 'b', 'c');
    REQUIRE_SAME("100%% sure");
    REQUIRE(Format("%s", static_cast<const char*>(nullptr)) == "(null)");
}

TEST_CASE("printf takes width and precision from the arguments")
{
    REQUIRE_SAME("[%*d] [%-*d] [%*d]", 5, 42, 5, 42, -5, 42);
    REQUIRE_SAME("[%.*d] [%.*s]", 4, 7, 2, "feather");
    REQUIRE_SAME("[%*.*s]", 8, 3, "feather");
    // A negative precision counts as none.
    REQUIRE_SAME("[%.*s]", -1, "feather");
}

TEST_CASE("printf prints pointers in hexadecimal and null as (nil)")
{
    int value = 0;
    REQUIRE_SAME("%p", static_cast<void*>(&value));
    REQUIRE(Format("%p", reinterpret_cast<void*>(0x1234)) == "0x1234");
    REQUIRE(Format("[%8p]", static_cast<void*>(nullptr)) == "[   (nil)]");
}

TEST_CASE("printf prints unsupported conversions as written")
{
    // %f consumes no argument, so the integer still reaches %d.
    REQUIRE(Format("%f|%d", 5) == "%f|5");
    REQUIRE(Format("%.2e %n") == "%.2e %n");
    REQUIRE(Format("trailing %") == "trailing %");
    REQUIRE(Format("%y") == "%y");
}

TEST_CASE("snprintf keeps what fits and reports the full length")
{
    char buffer[8];
    REQUIRE(Truncated(buffer, sizeof(buffer), "%s=%d", "value", 12345) == 11);
    REQUIRE(std::string(buffer) == "value=1");
    REQUIRE(Truncated(buffer, sizeof(buffer), "%d", 42) == 2);
    REQUIRE(std::string(buffer) == "42");
    buffer[0] = 'x';
    REQUIRE(Truncated(buffer, 1, "abc") == 3);
    REQUIRE(buffer[0] == '\0');
    REQUIRE(Truncated(nullptr, 0, "%d", 123456) == 6);
}
