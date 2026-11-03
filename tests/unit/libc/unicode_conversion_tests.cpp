#include <catch2/catch_test_macros.hpp>

#include "libc/unicode_conversion.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace
{

using FeatherOS::LibC::ConversionIncomplete;
using FeatherOS::LibC::ConversionInvalid;
using FeatherOS::LibC::ConversionPending;
using FeatherOS::LibC::ConversionState;
using FeatherOS::LibC::DecodeUtf8;
using FeatherOS::LibC::DecodeUtf8ToUtf16;
using FeatherOS::LibC::EncodeUtf16AsUtf8;
using FeatherOS::LibC::EncodeUtf8;

uint32_t DecodeWhole(const std::string& bytes, size_t* used)
{
    ConversionState state = {};
    uint32_t character = 0xFFFFFFFF;
    *used = DecodeUtf8(state, bytes.data(), bytes.size(), &character);
    return character;
}

std::string Encode(uint32_t character)
{
    char bytes[4] = {};
    const size_t length = EncodeUtf8(character, bytes);
    return length == ConversionInvalid ? std::string("invalid") : std::string(bytes, length);
}

} // namespace

TEST_CASE("UTF-8 decoding reads one character of each length")
{
    size_t used = 0;
    REQUIRE(DecodeWhole("A!", &used) == 'A');
    REQUIRE(used == 1);
    REQUIRE(DecodeWhole("\xC3\xA9x", &used) == 0xE9);
    REQUIRE(used == 2);
    REQUIRE(DecodeWhole("\xE2\x82\xAC", &used) == 0x20AC);
    REQUIRE(used == 3);
    REQUIRE(DecodeWhole("\xF0\x9F\x98\x80", &used) == 0x1F600);
    REQUIRE(used == 4);
    REQUIRE(DecodeWhole(std::string(1, '\0'), &used) == 0);
    REQUIRE(used == 0);
}

TEST_CASE("UTF-8 decoding carries a character across calls")
{
    ConversionState state = {};
    uint32_t character = 0;
    const char bytes[] = "\xF0\x9F\x98\x80";
    REQUIRE(DecodeUtf8(state, bytes, 1, &character) == ConversionIncomplete);
    REQUIRE(DecodeUtf8(state, bytes + 1, 2, &character) == ConversionIncomplete);
    REQUIRE(DecodeUtf8(state, bytes + 3, 1, &character) == 1);
    REQUIRE(character == 0x1F600);
    REQUIRE(state.needed == 0);
}

TEST_CASE("UTF-8 decoding refuses malformed sequences")
{
    size_t used = 0;
    DecodeWhole("\x80", &used);
    REQUIRE(used == ConversionInvalid);
    DecodeWhole("\xC0\xAF", &used);
    REQUIRE(used == ConversionInvalid);
    DecodeWhole("\xE0\x80\xAF", &used);
    REQUIRE(used == ConversionInvalid);
    DecodeWhole("\xED\xA0\x80", &used);
    REQUIRE(used == ConversionInvalid);
    DecodeWhole("\xF4\x90\x80\x80", &used);
    REQUIRE(used == ConversionInvalid);
    DecodeWhole("\xF5\x80\x80\x80", &used);
    REQUIRE(used == ConversionInvalid);
    DecodeWhole("\xC3(", &used);
    REQUIRE(used == ConversionInvalid);
}

TEST_CASE("UTF-8 encoding writes the shortest form")
{
    REQUIRE(Encode('A') == "A");
    REQUIRE(Encode(0x7F) == "\x7F");
    REQUIRE(Encode(0x80) == "\xC2\x80");
    REQUIRE(Encode(0x7FF) == "\xDF\xBF");
    REQUIRE(Encode(0x800) == "\xE0\xA0\x80");
    REQUIRE(Encode(0xFFFF) == "\xEF\xBF\xBF");
    REQUIRE(Encode(0x10000) == "\xF0\x90\x80\x80");
    REQUIRE(Encode(0x10FFFF) == "\xF4\x8F\xBF\xBF");
    REQUIRE(Encode(0xD800) == "invalid");
    REQUIRE(Encode(0x110000) == "invalid");
}

TEST_CASE("UTF-8 to UTF-16 returns a surrogate pair over two calls")
{
    ConversionState state = {};
    uint16_t unit = 0;
    const char bytes[] = "\xF0\x9F\x98\x80z";
    REQUIRE(DecodeUtf8ToUtf16(state, bytes, 5, &unit) == 4);
    REQUIRE(unit == 0xD83D);
    REQUIRE(DecodeUtf8ToUtf16(state, bytes + 4, 1, &unit) == ConversionPending);
    REQUIRE(unit == 0xDE00);
    REQUIRE(DecodeUtf8ToUtf16(state, bytes + 4, 1, &unit) == 1);
    REQUIRE(unit == 'z');
}

TEST_CASE("UTF-16 to UTF-8 joins a surrogate pair")
{
    ConversionState state = {};
    char bytes[4] = {};
    REQUIRE(EncodeUtf16AsUtf8(state, 0xD83D, bytes) == 0);
    REQUIRE(EncodeUtf16AsUtf8(state, 0xDE00, bytes) == 4);
    REQUIRE(std::memcmp(bytes, "\xF0\x9F\x98\x80", 4) == 0);
    REQUIRE(EncodeUtf16AsUtf8(state, 0xE9, bytes) == 2);
    REQUIRE(EncodeUtf16AsUtf8(state, 0xDE00, bytes) == ConversionInvalid);
    REQUIRE(EncodeUtf16AsUtf8(state, 0xD83D, bytes) == 0);
    REQUIRE(EncodeUtf16AsUtf8(state, 'a', bytes) == ConversionInvalid);
}
