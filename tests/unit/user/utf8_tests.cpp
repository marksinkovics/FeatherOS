#include <catch2/catch_test_macros.hpp>

#include "user/text/utf8.h"

#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace Text = FeatherOS::Text;
using Text::DecodeNext;
using Text::Utf8Decoder;

namespace
{

constexpr char32_t Replacement = 0xFFFD;

std::vector<char32_t> DecodeWhole(const std::string& bytes)
{
    std::vector<char32_t> out;
    size_t offset = 0;
    while (offset < bytes.size())
    {
        size_t used = 0;
        out.push_back(DecodeNext(bytes.data() + offset, bytes.size() - offset, &used));
        REQUIRE(used > 0);
        offset += used;
    }
    return out;
}

std::vector<char32_t> DecodeStream(const std::string& bytes)
{
    std::vector<char32_t> out;
    Utf8Decoder decoder;
    for (const char byte : bytes)
    {
        char32_t produced[2] = {0, 0};
        const size_t count = decoder.Push(static_cast<uint8_t>(byte), produced);
        REQUIRE(count <= 2);
        for (size_t index = 0; index < count; ++index)
        {
            out.push_back(produced[index]);
        }
    }
    char32_t last[2] = {0, 0};
    const size_t count = decoder.Finish(last);
    for (size_t index = 0; index < count; ++index)
    {
        out.push_back(last[index]);
    }
    return out;
}

std::string Bytes(std::initializer_list<int> values)
{
    std::string out;
    for (const int value : values)
    {
        out.push_back(static_cast<char>(value));
    }
    return out;
}

// Unicode's Table 3-7 as data, read the long way round: the longest prefix that is the start of
// a well-formed sequence becomes one U+FFFD.
struct Form
{
    int lead_low;
    int lead_high;
    int length;
    int second_low;
    int second_high;
};

constexpr Form Forms[] = {
    {0xC2, 0xDF, 2, 0x80, 0xBF}, {0xE0, 0xE0, 3, 0xA0, 0xBF}, {0xE1, 0xEC, 3, 0x80, 0xBF},
    {0xED, 0xED, 3, 0x80, 0x9F}, {0xEE, 0xEF, 3, 0x80, 0xBF}, {0xF0, 0xF0, 4, 0x90, 0xBF},
    {0xF1, 0xF3, 4, 0x80, 0xBF}, {0xF4, 0xF4, 4, 0x80, 0x8F},
};

std::vector<char32_t> Reference(const std::string& bytes)
{
    std::vector<char32_t> out;
    size_t index = 0;
    const auto at = [&](size_t position)
    {
        return position < bytes.size() ? static_cast<int>(static_cast<uint8_t>(bytes[position]))
                                       : -1;
    };
    while (index < bytes.size())
    {
        const int lead = at(index);
        if (lead < 0x80)
        {
            out.push_back(static_cast<char32_t>(lead));
            ++index;
            continue;
        }
        const Form* form = nullptr;
        for (const Form& candidate : Forms)
        {
            if (lead >= candidate.lead_low && lead <= candidate.lead_high)
            {
                form = &candidate;
            }
        }
        if (form == nullptr)
        {
            out.push_back(Replacement);
            ++index;
            continue;
        }
        size_t taken = 1;
        char32_t code = static_cast<char32_t>(lead & (0xFF >> (form->length + 1)));
        bool complete = true;
        for (int position = 1; position < form->length; ++position)
        {
            const int next = at(index + taken);
            const int low = position == 1 ? form->second_low : 0x80;
            const int high = position == 1 ? form->second_high : 0xBF;
            if (next < low || next > high)
            {
                complete = false;
                break;
            }
            code = (code << 6) | static_cast<char32_t>(next & 0x3F);
            ++taken;
        }
        out.push_back(complete ? code : Replacement);
        index += taken;
    }
    return out;
}

std::string RandomBytes(std::mt19937& generator)
{
    static const int Interesting[] = {0x00, 0x41, 0x7F, 0x80, 0x8F, 0x90, 0x9F, 0xA0, 0xBF,
                                      0xC0, 0xC1, 0xC2, 0xDF, 0xE0, 0xE1, 0xEC, 0xED, 0xEE,
                                      0xEF, 0xF0, 0xF1, 0xF3, 0xF4, 0xF5, 0xFF};
    std::string out;
    const size_t length = generator() % 24;
    for (size_t index = 0; index < length; ++index)
    {
        const bool plain = generator() % 3 == 0;
        const int value = plain ? static_cast<int>(generator() % 256)
                                : Interesting[generator() % (sizeof(Interesting) / sizeof(int))];
        out.push_back(static_cast<char>(value));
    }
    return out;
}

} // namespace

TEST_CASE("valid forms of one to four bytes decode", "[utf8]")
{
    CHECK(DecodeWhole("A") == std::vector<char32_t>{0x41});
    CHECK(DecodeWhole(Bytes({0xC3, 0xA9})) == std::vector<char32_t>{0xE9});
    CHECK(DecodeWhole(Bytes({0xE2, 0x82, 0xAC})) == std::vector<char32_t>{0x20AC});
    CHECK(DecodeWhole(Bytes({0xF0, 0x9F, 0x98, 0x80})) == std::vector<char32_t>{0x1F600});
    CHECK(DecodeWhole(Bytes({0x7F})) == std::vector<char32_t>{0x7F});
    CHECK(DecodeWhole(Bytes({0xC2, 0x80})) == std::vector<char32_t>{0x80});
    CHECK(DecodeWhole(Bytes({0xDF, 0xBF})) == std::vector<char32_t>{0x7FF});
    CHECK(DecodeWhole(Bytes({0xE0, 0xA0, 0x80})) == std::vector<char32_t>{0x800});
    CHECK(DecodeWhole(Bytes({0xEF, 0xBF, 0xBF})) == std::vector<char32_t>{0xFFFF});
    CHECK(DecodeWhole(Bytes({0xEF, 0xBF, 0xBD})) == std::vector<char32_t>{0xFFFD});
    CHECK(DecodeWhole(Bytes({0xF0, 0x90, 0x80, 0x80})) == std::vector<char32_t>{0x10000});
    CHECK(DecodeWhole(Bytes({0xF4, 0x8F, 0xBF, 0xBF})) == std::vector<char32_t>{0x10FFFF});
    CHECK(DecodeWhole(Bytes({0xED, 0x9F, 0xBF})) == std::vector<char32_t>{0xD7FF});
    CHECK(DecodeWhole(Bytes({0xEE, 0x80, 0x80})) == std::vector<char32_t>{0xE000});
    CHECK(DecodeWhole(Bytes({0x00})) == std::vector<char32_t>{0});
}

TEST_CASE("DecodeNext reports the bytes it used", "[utf8]")
{
    size_t used = 99;
    CHECK(DecodeNext("", 0, &used) == 0);
    CHECK(used == 0);
    const std::string text = Bytes({0xE2, 0x82, 0xAC, 'x'});
    CHECK(DecodeNext(text.data(), text.size(), &used) == 0x20AC);
    CHECK(used == 3);
    CHECK(DecodeNext(text.data(), 2, &used) == Replacement);
    CHECK(used == 2);
}

TEST_CASE("overlong forms are refused at the byte where they go wrong", "[utf8]")
{
    CHECK(DecodeWhole(Bytes({0xC0, 0x80})) == std::vector<char32_t>{Replacement, Replacement});
    CHECK(DecodeWhole(Bytes({0xC1, 0xBF})) == std::vector<char32_t>{Replacement, Replacement});
    CHECK(DecodeWhole(Bytes({0xE0, 0x80, 0x80})) ==
          std::vector<char32_t>{Replacement, Replacement, Replacement});
    CHECK(DecodeWhole(Bytes({0xE0, 0x9F, 0xBF})) ==
          std::vector<char32_t>{Replacement, Replacement, Replacement});
    CHECK(DecodeWhole(Bytes({0xF0, 0x80, 0x80, 0x80})) ==
          std::vector<char32_t>{Replacement, Replacement, Replacement, Replacement});
    CHECK(DecodeWhole(Bytes({0xF0, 0x8F, 0xBF, 0xBF})) ==
          std::vector<char32_t>{Replacement, Replacement, Replacement, Replacement});
    CHECK(DecodeWhole(Bytes({0xE0, 0xA0, 0x80})) == std::vector<char32_t>{0x800});
}

TEST_CASE("surrogates and values past U+10FFFF are refused", "[utf8]")
{
    CHECK(DecodeWhole(Bytes({0xED, 0xA0, 0x80})) ==
          std::vector<char32_t>{Replacement, Replacement, Replacement});
    CHECK(DecodeWhole(Bytes({0xED, 0xBF, 0xBF})) ==
          std::vector<char32_t>{Replacement, Replacement, Replacement});
    CHECK(DecodeWhole(Bytes({0xF4, 0x90, 0x80, 0x80})) ==
          std::vector<char32_t>{Replacement, Replacement, Replacement, Replacement});
    for (int lead = 0xF5; lead <= 0xFF; ++lead)
    {
        CHECK(DecodeWhole(Bytes({lead})) == std::vector<char32_t>{Replacement});
        CHECK(DecodeWhole(Bytes({lead, 0x80, 0x80, 0x80})) ==
              std::vector<char32_t>{Replacement, Replacement, Replacement, Replacement});
    }
}

TEST_CASE("a stray continuation byte is one replacement", "[utf8]")
{
    CHECK(DecodeWhole(Bytes({0x80})) == std::vector<char32_t>{Replacement});
    CHECK(DecodeWhole(Bytes({0xBF, 0xBF})) == std::vector<char32_t>{Replacement, Replacement});
}

TEST_CASE("truncated sequences give one replacement", "[utf8]")
{
    CHECK(DecodeWhole(Bytes({0xE2, 0x82})) == std::vector<char32_t>{Replacement});
    CHECK(DecodeWhole(Bytes({0xF0, 0x9F, 0x98})) == std::vector<char32_t>{Replacement});
    CHECK(DecodeWhole(Bytes({0xC3})) == std::vector<char32_t>{Replacement});
    CHECK(DecodeWhole(Bytes({'a', 0xE2, 0x82})) == std::vector<char32_t>{'a', Replacement});
    CHECK(DecodeStream(Bytes({0xE2, 0x82})) == std::vector<char32_t>{Replacement});
}

TEST_CASE("a byte that breaks a sequence is read again", "[utf8]")
{
    CHECK(DecodeWhole(Bytes({0xE2, 'a'})) == std::vector<char32_t>{Replacement, 'a'});
    CHECK(DecodeWhole(Bytes({0xE2, 0x82, 'b'})) == std::vector<char32_t>{Replacement, 'b'});
    CHECK(DecodeWhole(Bytes({0xF0, 0x9F, 0xC3, 0xA9})) == std::vector<char32_t>{Replacement, 0xE9});
    CHECK(DecodeWhole(Bytes({0xE2, 0xE2, 0x82, 0xAC})) ==
          std::vector<char32_t>{Replacement, 0x20AC});
    CHECK(DecodeWhole(Bytes({0xC3, '\n'})) == std::vector<char32_t>{Replacement, '\n'});
    CHECK(DecodeStream(Bytes({0xE2, 'a'})) == std::vector<char32_t>{Replacement, 'a'});
    CHECK(DecodeStream(Bytes({0xE2, 0xFF})) == std::vector<char32_t>{Replacement, Replacement});
}

TEST_CASE("the streaming decoder keeps state across calls and reports pending input", "[utf8]")
{
    Utf8Decoder decoder;
    char32_t out[2] = {0, 0};
    CHECK_FALSE(decoder.Pending());
    CHECK(decoder.Push(0xE2, out) == 0);
    CHECK(decoder.Pending());
    CHECK(decoder.Push(0x82, out) == 0);
    CHECK(decoder.Push(0xAC, out) == 1);
    CHECK(out[0] == 0x20AC);
    CHECK_FALSE(decoder.Pending());
    CHECK(decoder.Finish(out) == 0);
    CHECK(decoder.Push(0xE2, out) == 0);
    CHECK(decoder.Finish(out) == 1);
    CHECK(out[0] == Replacement);
    CHECK_FALSE(decoder.Pending());
}

TEST_CASE("validity agrees with decoding", "[utf8]")
{
    const std::string valid = Bytes({'a', 0xC3, 0xA9, 0xEF, 0xBF, 0xBD, 0xF0, 0x9F, 0x98, 0x80});
    CHECK(Text::IsValidUtf8(valid.data(), valid.size()));
    CHECK(Text::IsValidUtf8("", 0));
    for (const std::string& bad : {Bytes({0xC0, 0x80}), Bytes({0xED, 0xA0, 0x80}),
                                   Bytes({0xE2, 0x82}), Bytes({0xFF}), Bytes({0x80})})
    {
        CHECK_FALSE(Text::IsValidUtf8(bad.data(), bad.size()));
    }
}

TEST_CASE("byte-at-a-time and whole-string decoding match a table-driven reference", "[utf8]")
{
    std::mt19937 generator(20261121);
    for (int round = 0; round < 10000; ++round)
    {
        const std::string bytes = RandomBytes(generator);
        const std::vector<char32_t> expected = Reference(bytes);
        INFO("round " << round << " of " << bytes.size() << " bytes");
        CHECK(DecodeWhole(bytes) == expected);
        CHECK(DecodeStream(bytes) == expected);
    }
}
