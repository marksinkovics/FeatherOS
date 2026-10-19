#include <catch2/catch_test_macros.hpp>

#include "kernel/string/format.h"

#include <limits>

TEST_CASE("formatter handles scalar values and escaped braces")
{
    char output[128];
    REQUIRE(FeatherOS::String::FormatTo(output, sizeof(output),
                                        "{{{}}} {} {} {} {}", "ok", true, 'x', -42, 42U));
    REQUIRE(std::string(output) == "{ok} true x -42 42");
}

TEST_CASE("formatter handles integer limits and null strings")
{
    char output[128];
    REQUIRE(FeatherOS::String::FormatTo(
        output, sizeof(output), "{} {} {}", std::numeric_limits<long long>::min(),
        std::numeric_limits<unsigned long long>::max(), static_cast<const char*>(nullptr)));
    REQUIRE(std::string(output) == "-9223372036854775808 18446744073709551615 (null)");
}

TEST_CASE("formatter rejects malformed braces and argument counts")
{
    char output[32];
    REQUIRE_FALSE(FeatherOS::String::FormatTo(output, sizeof(output), "unmatched {"));
    REQUIRE_FALSE(FeatherOS::String::FormatTo(output, sizeof(output), "missing {}"));
    REQUIRE_FALSE(FeatherOS::String::FormatTo(output, sizeof(output), "extra", 1));
    REQUIRE_FALSE(FeatherOS::String::FormatTo(output, sizeof(output), "{}", 1, 2));
}

TEST_CASE("formatter fails cleanly when output capacity is insufficient")
{
    char output[4];
    REQUIRE_FALSE(FeatherOS::String::FormatTo(output, sizeof(output), "{}", "long"));
    REQUIRE(output[sizeof(output) - 1] == '\0');
    REQUIRE_FALSE(FeatherOS::String::FormatTo(nullptr, 4, "value"));
    REQUIRE_FALSE(FeatherOS::String::FormatTo(output, 0, "value"));
    REQUIRE_FALSE(FeatherOS::String::FormatTo(output, sizeof(output), nullptr));
}