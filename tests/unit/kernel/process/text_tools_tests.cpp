#include <catch2/catch_test_macros.hpp>

#include "libc/regex_engine.h"
#include "user/text_tools.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace Regex = FeatherOS::LibC::Regex;

using FeatherOS::User::CompareLines;
using FeatherOS::User::CountText;
using FeatherOS::User::FixedTextPattern;
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

TEST_CASE("grep -F escapes plain text into a basic regular expression")
{
    const std::string special = "a.b[c]\\d*e^f$g+h?(i){j}|k";
    char* pattern = FixedTextPattern(special.c_str());
    REQUIRE(pattern != nullptr);
    REQUIRE(std::string(pattern) == "a\\.b\\[c]\\\\d\\*e\\^f\\$g+h?(i){j}|k");
    Regex::Program* program = nullptr;
    size_t count = 0;
    REQUIRE(Regex::Compile(pattern, 0, &program, &count) == Regex::Status::Matched);
    free(pattern);
    const std::string text = "x" + special + "x";
    Regex::Span span{};
    REQUIRE(Regex::Execute(*program, text.data(), text.size(), 0, &span, 1) ==
            Regex::Status::Matched);
    REQUIRE((span.start == 1 && span.end == static_cast<long>(special.size() + 1)));
    REQUIRE(Regex::Execute(*program, "a.b", 3, 0, nullptr, 0) == Regex::Status::NoMatch);
    Regex::Free(program);
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
    bool letters[128] = {};
    char bad = 0;
    REQUIRE(ParseFlags(5, argv, "icnv", letters, &bad) == 4);
    REQUIRE(letters['i']);
    REQUIRE(letters['c']);
    REQUIRE(letters['n']);
    REQUIRE_FALSE(letters['v']);
    char* unknown[] = {first, pattern};
    REQUIRE(ParseFlags(2, unknown, "icnv", letters, &bad) == -1);
    REQUIRE(bad == 'x');
    char upper[] = "-Ei";
    char* mixed[] = {first, upper};
    bool more[128] = {};
    REQUIRE(ParseFlags(2, mixed, "EFi", more, &bad) == 2);
    REQUIRE((more['E'] && more['i'] && !more['F']));
    REQUIRE(ParseFlags(2, mixed, "Fi", more, &bad) == -1);
    REQUIRE(bad == 'E');
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
