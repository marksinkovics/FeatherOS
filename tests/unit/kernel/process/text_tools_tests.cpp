#include <catch2/catch_test_macros.hpp>

#include "user/text_tools.h"

#include <cstring>
#include <string>

using FeatherOS::User::CompareLines;
using FeatherOS::User::ContainsText;
using FeatherOS::User::CountText;
using FeatherOS::User::LastLines;
using FeatherOS::User::ParseFlags;
using FeatherOS::User::TextCounts;

TEST_CASE("wc counts lines, words and bytes across split reads")
{
    TextCounts counts = {0, 0, 0};
    bool in_word = false;
    const std::string first = "one two\n  thr";
    const std::string second = "ee\tfour\n\nfive";
    CountText(first.data(), first.size(), &counts, &in_word);
    CountText(second.data(), second.size(), &counts, &in_word);
    REQUIRE(counts.lines == 3);
    REQUIRE(counts.words == 5);
    REQUIRE(counts.bytes == first.size() + second.size());
}

TEST_CASE("grep matches plain text, with or without case")
{
    REQUIRE(ContainsText("hello world", "lo w", false));
    REQUIRE(ContainsText("anything", "", false));
    REQUIRE_FALSE(ContainsText("Hello", "hello", false));
    REQUIRE(ContainsText("Hello", "hELLO", true));
    REQUIRE_FALSE(ContainsText("short", "shorter", true));
    REQUIRE(ContainsText("aaab", "aab", false));
}

TEST_CASE("sort compares by bytes or by leading number")
{
    REQUIRE(CompareLines("alpha", "beta", false) < 0);
    REQUIRE(CompareLines("10", "9", false) < 0);
    REQUIRE(CompareLines("10", "9", true) > 0);
    REQUIRE(CompareLines("  -3 apples", "2", true) < 0);
    // Lines without a number count as 0, and ties fall back to bytes.
    REQUIRE(CompareLines("pear", "0", true) > 0);
    REQUIRE(CompareLines("1.5", "1.25", true) > 0);
    REQUIRE(CompareLines("7 b", "7 a", true) > 0);
}

TEST_CASE("flags are read until the first operand")
{
    char first[] = "grep";
    char combined[] = "-ic";
    char single[] = "-n";
    char pattern[] = "-x";
    char end[] = "--";
    char* argv[] = {first, combined, single, end, pattern};
    bool letters[26] = {};
    char bad = 0;
    REQUIRE(ParseFlags(5, argv, "icnv", letters, &bad) == 4);
    REQUIRE(letters['i' - 'a']);
    REQUIRE(letters['c' - 'a']);
    REQUIRE(letters['n' - 'a']);
    REQUIRE_FALSE(letters['v' - 'a']);
    char* unknown[] = {first, pattern};
    REQUIRE(ParseFlags(2, unknown, "icnv", letters, &bad) == -1);
    REQUIRE(bad == 'x');
}

TEST_CASE("tail keeps the last lines in order")
{
    LastLines last(3);
    for (const char* line : {"1", "2", "3", "4", "5"})
    {
        REQUIRE(last.Add(line));
    }
    REQUIRE(last.Count() == 3);
    REQUIRE(std::strcmp(last.Line(0), "3") == 0);
    REQUIRE(std::strcmp(last.Line(2), "5") == 0);
    LastLines none(0);
    REQUIRE(none.Add("x"));
    REQUIRE(none.Count() == 0);
}
