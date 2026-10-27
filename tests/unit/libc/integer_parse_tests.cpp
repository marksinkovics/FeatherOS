#include <catch2/catch_test_macros.hpp>

#include "libc/integer_parse.h"

#include <cstdint>

namespace
{

using FeatherOS::LibC::ParsedInteger;
using FeatherOS::LibC::ParseInteger;

constexpr uint64_t SignedMaximum = INT64_MAX;
constexpr uint64_t SignedMinimumMagnitude = SignedMaximum + 1;

ParsedInteger ParseSigned(const char* text, int base = 10)
{
    return ParseInteger(text, base, SignedMaximum, SignedMinimumMagnitude);
}

} // namespace

TEST_CASE("integer parsing skips spaces and reads a sign")
{
    const char text[] = " \t\n-42abc";
    const ParsedInteger parsed = ParseSigned(text);
    REQUIRE(parsed.magnitude == 42);
    REQUIRE(parsed.negative);
    REQUIRE_FALSE(parsed.overflow);
    REQUIRE(parsed.end == text + 6);
    REQUIRE(ParseSigned("+7").magnitude == 7);
    REQUIRE_FALSE(ParseSigned("+7").negative);
}

TEST_CASE("integer parsing reports no digits by ending at the start")
{
    const char letters[] = "xyz";
    REQUIRE(ParseSigned(letters).end == letters);
    const char sign_only[] = "  -";
    REQUIRE(ParseSigned(sign_only).end == sign_only);
    const char empty[] = "";
    REQUIRE(ParseSigned(empty).end == empty);
}

TEST_CASE("base 0 picks hexadecimal, octal, or decimal from the prefix")
{
    REQUIRE(ParseSigned("0x1f", 0).magnitude == 31);
    REQUIRE(ParseSigned("0X1F", 0).magnitude == 31);
    REQUIRE(ParseSigned("017", 0).magnitude == 15);
    REQUIRE(ParseSigned("0", 0).magnitude == 0);
    REQUIRE(ParseSigned("19", 0).magnitude == 19);
    // 8 and 9 are not octal digits, so parsing stops after the leading 0.
    const char not_octal[] = "09";
    REQUIRE(ParseSigned(not_octal, 0).end == not_octal + 1);
}

TEST_CASE("0x is a prefix only when a hexadecimal digit follows")
{
    const char bare[] = "0x";
    const ParsedInteger parsed = ParseSigned(bare, 16);
    REQUIRE(parsed.magnitude == 0);
    REQUIRE(parsed.end == bare + 1);
    const char not_hex[] = "0xg";
    REQUIRE(ParseSigned(not_hex, 0).end == not_hex + 1);
    REQUIRE(ParseSigned("0x10", 16).magnitude == 16);
    // In base 10 the x ends the number.
    REQUIRE(ParseSigned("0x10", 10).magnitude == 0);
}

TEST_CASE("digits above 9 are letters in either case")
{
    REQUIRE(ParseSigned("z", 36).magnitude == 35);
    REQUIRE(ParseSigned("Zz", 36).magnitude == 35 * 36 + 35);
    REQUIRE(ParseSigned("101", 2).magnitude == 5);
    const char binary[] = "1012";
    REQUIRE(ParseSigned(binary, 2).end == binary + 3);
}

TEST_CASE("integer parsing saturates at the limit for the sign")
{
    REQUIRE(ParseSigned("9223372036854775807").magnitude == SignedMaximum);
    REQUIRE_FALSE(ParseSigned("9223372036854775807").overflow);

    const char too_large[] = "9223372036854775808";
    const ParsedInteger positive = ParseSigned(too_large);
    REQUIRE(positive.overflow);
    REQUIRE(positive.magnitude == SignedMaximum);
    REQUIRE(positive.end == too_large + 19);

    const ParsedInteger minimum = ParseSigned("-9223372036854775808");
    REQUIRE_FALSE(minimum.overflow);
    REQUIRE(minimum.magnitude == SignedMinimumMagnitude);
    REQUIRE(ParseSigned("-9223372036854775809").overflow);

    const ParsedInteger unsigned_maximum =
        ParseInteger("18446744073709551615", 10, UINT64_MAX, UINT64_MAX);
    REQUIRE_FALSE(unsigned_maximum.overflow);
    REQUIRE(unsigned_maximum.magnitude == UINT64_MAX);
    REQUIRE(ParseInteger("18446744073709551616", 10, UINT64_MAX, UINT64_MAX).overflow);
    REQUIRE(ParseInteger("ffffffffffffffff0", 16, UINT64_MAX, UINT64_MAX).overflow);
}

TEST_CASE("valid bases are 0 and 2 through 36")
{
    REQUIRE(FeatherOS::LibC::IsValidBase(0));
    REQUIRE(FeatherOS::LibC::IsValidBase(2));
    REQUIRE(FeatherOS::LibC::IsValidBase(36));
    REQUIRE_FALSE(FeatherOS::LibC::IsValidBase(1));
    REQUIRE_FALSE(FeatherOS::LibC::IsValidBase(37));
    REQUIRE_FALSE(FeatherOS::LibC::IsValidBase(-1));
}
