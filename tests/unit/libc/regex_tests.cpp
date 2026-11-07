#include <catch2/catch_test_macros.hpp>

#include "libc/regex_engine.h"

#include <string>
#include <vector>

namespace Regex = FeatherOS::LibC::Regex;

namespace
{

struct Result
{
    Regex::Status status;
    // "start-end" for each span, "-" for one that took no part.
    std::vector<std::string> spans;
};

Regex::Status CompileStatus(const char* pattern, int flags)
{
    Regex::Program* program = nullptr;
    size_t count = 0;
    const Regex::Status status = Regex::Compile(pattern, flags, &program, &count);
    Regex::Free(program);
    return status;
}

Result Run(const char* pattern, int flags, const std::string& text, int execute_flags = 0,
           size_t count = 10)
{
    Regex::Program* program = nullptr;
    size_t subexpressions = 0;
    Result result{Regex::Compile(pattern, flags, &program, &subexpressions), {}};
    if (result.status != Regex::Status::Matched)
    {
        return result;
    }
    std::vector<Regex::Span> spans(count);
    result.status =
        Regex::Execute(*program, text.data(), text.size(), execute_flags, spans.data(), count);
    if (result.status == Regex::Status::Matched)
    {
        for (size_t index = 0; index < count && index <= subexpressions; ++index)
        {
            const Regex::Span& span = spans[index];
            result.spans.push_back(span.start < 0 ? "-"
                                                  : std::to_string(span.start) + "-" +
                                                        std::to_string(span.end));
        }
    }
    Regex::Free(program);
    return result;
}

std::vector<std::string> Spans(const char* pattern, int flags, const std::string& text)
{
    const Result result = Run(pattern, flags, text);
    REQUIRE(result.status == Regex::Status::Matched);
    return result.spans;
}

bool Matches(const char* pattern, int flags, const std::string& text)
{
    return Run(pattern, flags, text, 0, 0).status == Regex::Status::Matched;
}

using Strings = std::vector<std::string>;

constexpr int Basic = 0;
constexpr int Extended = Regex::Extended;

} // namespace

TEST_CASE("regex matches literals, any byte and anchors")
{
    CHECK(Spans("feather", Basic, "a feather os") == Strings{"2-9"});
    CHECK(Spans("f.a", Basic, "the feather") == Strings{"4-7"});
    CHECK(Spans("", Basic, "abc") == Strings{"0-0"});
    CHECK(Spans("^ab", Basic, "abab") == Strings{"0-2"});
    CHECK(Spans("ab$", Basic, "abab") == Strings{"2-4"});
    CHECK_FALSE(Matches("^b", Basic, "ab"));
    CHECK_FALSE(Matches("a$", Basic, "ab"));
    CHECK_FALSE(Matches("x", Basic, "abc"));
}

TEST_CASE("regex finds the leftmost match, and the longest from there")
{
    CHECK(Spans("a|ab", Extended, "xabc") == Strings{"1-3"});
    CHECK(Spans("b+|a+b*", Extended, "aabbb") == Strings{"0-5"});
    CHECK(Spans("(a|ab)(c|bcd)", Extended, "abcd") == Strings{"0-4", "0-1", "1-4"});
    CHECK(Spans("x*", Extended, "aaxx") == Strings{"0-0"});
    CHECK(Spans("a*", Basic, "aaab") == Strings{"0-3"});
}

TEST_CASE("regex subexpressions take the first assignment that gives the match")
{
    CHECK(Spans("(a*)(b)", Extended, "aab") == Strings{"0-3", "0-2", "2-3"});
    CHECK(Spans("(a)|(b)", Extended, "b") == Strings{"0-1", "-", "0-1"});
    // A repeated subexpression keeps its last iteration.
    CHECK(Spans("(a|b)*", Extended, "abab") == Strings{"0-4", "3-4"});
    // What is nested in it reports only what matched within that last iteration.
    CHECK(Spans("(a(b)?)+", Extended, "aba") == Strings{"0-3", "2-3", "-"});
    CHECK(Spans("(a|(b))*", Extended, "ba") == Strings{"0-2", "1-2", "-"});
    CHECK(Spans("(a(b)?)+", Extended, "aab") == Strings{"0-3", "1-3", "2-3"});
    // POSIX would give the first subexpression "ab" and the second "c", the longest for each in
    // turn; the search tries alternatives left to right instead.
    CHECK(Spans("(a|ab)(c|bcd)(d*)", Extended, "abcd") == Strings{"0-4", "0-1", "1-4", "4-4"});
    CHECK(Spans("()", Extended, "x") == Strings{"0-0", "0-0"});
}

TEST_CASE("regex repetitions and intervals")
{
    CHECK(Spans("ab+c", Extended, "abbbc") == Strings{"0-5"});
    CHECK_FALSE(Matches("ab+c", Extended, "ac"));
    CHECK(Spans("colou?r", Extended, "color") == Strings{"0-5"});
    CHECK(Spans("a{2,3}", Extended, "aaaa") == Strings{"0-3"});
    CHECK(Spans("a{2}", Extended, "aaaa") == Strings{"0-2"});
    CHECK(Spans("a{2,}", Extended, "aaaa") == Strings{"0-4"});
    CHECK(Spans("xa{0}y", Extended, "xy") == Strings{"0-2"});
    CHECK(Spans("(ab){1,2}", Extended, "ababab") == Strings{"0-4", "2-4"});
    CHECK_FALSE(Matches("a{3}", Extended, "aa"));
    CHECK(Spans("a\\{2\\}", Basic, "aaa") == Strings{"0-2"});
    CHECK(Spans("a**", Extended, "aa") == Strings{"0-2"});
}

TEST_CASE("regex repetitions of what matches nothing end")
{
    CHECK(Spans("(a*)*", Extended, "aab") == Strings{"0-2", "0-2"});
    CHECK(Spans("(a*)+b", Extended, "b") == Strings{"0-1", "0-0"});
    CHECK(Spans("(|a)+", Extended, "aa") == Strings{"0-2", "1-2"});
    CHECK(Spans("(^)*x", Extended, "x") == Strings{"0-1", "-"});
}

TEST_CASE("regex stays linear where backtracking alone would not")
{
    const std::string as(5000, 'a');
    CHECK(Run("(a*)*b", Extended, as).status == Regex::Status::NoMatch);
    CHECK(Spans("(a|aa)*c", Extended, as + "c") == Strings{"0-5001", "4999-5000"});
}

TEST_CASE("regex bracket expressions")
{
    CHECK(Spans("[abc]+", Extended, "xxcabz") == Strings{"2-5"});
    CHECK(Spans("[^abc]", Basic, "abcd") == Strings{"3-4"});
    CHECK(Spans("[a-c]*", Basic, "abcd") == Strings{"0-3"});
    CHECK(Spans("[]a]*", Basic, "]a]b") == Strings{"0-3"});
    CHECK(Spans("[^]a]", Basic, "]ab") == Strings{"2-3"});
    CHECK(Spans("[a-]*", Basic, "a-b") == Strings{"0-2"});
    CHECK(Spans("[-a]*", Basic, "-ab") == Strings{"0-2"});
    CHECK(Spans("[[:digit:]]+", Extended, "ab123c") == Strings{"2-5"});
    CHECK(Spans("[[:alpha:][:space:]]+", Extended, "1a b2") == Strings{"1-4"});
    CHECK(Spans("[[:upper:]]", Extended, "abC") == Strings{"2-3"});
    CHECK(Spans("[[:punct:]]", Extended, "a,b") == Strings{"1-2"});
    CHECK(Spans("[[:xdigit:]]+", Extended, "xBeEfy") == Strings{"1-5"});
    CHECK(Spans("[[.a.]-c]+", Extended, "abc") == Strings{"0-3"});
    CHECK(Spans("[[=e=]]", Extended, "xe") == Strings{"1-2"});
    CHECK(Spans("[\\]+", Extended, "a\\\\") == Strings{"1-3"});
    CHECK(Spans("x[.*]*", Basic, "ax.*") == Strings{"1-4"});
}

TEST_CASE("regex basic syntax treats ERE operators as plain characters")
{
    CHECK(Spans("a+?|(b)", Basic, "xa+?|(b)") == Strings{"1-8"});
    CHECK(Spans("*a", Basic, "x*a") == Strings{"1-3"});
    CHECK(Spans("^*a", Basic, "*a") == Strings{"0-2"});
    CHECK(Spans("\\(*a\\)", Basic, "*a") == Strings{"0-2", "0-2"});
    CHECK(Spans("a^b", Basic, "a^b") == Strings{"0-3"});
    CHECK(Spans("a$b", Basic, "a$b") == Strings{"0-3"});
    CHECK(Spans("\\(^a$\\)", Basic, "a") == Strings{"0-1", "0-1"});
    CHECK(Spans("a{1}", Basic, "a{1}") == Strings{"0-4"});
    CHECK(Spans("\\.\\*\\[", Basic, "x.*[") == Strings{"1-4"});
    CHECK(Spans("a)", Extended, "a)") == Strings{"0-2"});
}

TEST_CASE("regex back-references")
{
    CHECK(Spans("\\(a*\\)b\\1", Basic, "aabaaa") == Strings{"0-5", "0-2"});
    CHECK(Spans("(.)\\1", Extended, "abccd") == Strings{"2-4", "2-3"});
    CHECK(Spans("\\(.*\\)-\\1", Basic, "x ab-ab-ab") == Strings{"2-7", "2-4"});
    CHECK_FALSE(Matches("(a)\\1", Extended, "ab"));
    CHECK(Spans("(a)\\1", Extended | Regex::IgnoreCase, "aA") == Strings{"0-2", "0-1"});
    CHECK(Spans("(a)|b\\1", Extended, "ba") == Strings{"1-2", "1-2"});
}

TEST_CASE("regex ignoring case")
{
    const int flags = Extended | Regex::IgnoreCase;
    CHECK(Spans("FeAtHeR", flags, "a feather") == Strings{"2-9"});
    CHECK(Spans("[a-c]+", flags, "xAbC") == Strings{"1-4"});
    CHECK(Spans("[^a]", flags, "Ab") == Strings{"1-2"});
    CHECK_FALSE(Matches("FEATHER", Extended, "feather"));
}

TEST_CASE("regex newline and the beginning and end flags")
{
    CHECK_FALSE(Matches("^b", Extended, "a\nb"));
    CHECK(Spans("^b$", Extended | Regex::Newline, "a\nb\nc") == Strings{"2-3"});
    CHECK(Spans("a.b", Extended, "a\nb") == Strings{"0-3"});
    CHECK_FALSE(Matches("a.b", Extended | Regex::Newline, "a\nb"));
    CHECK_FALSE(Matches("a[^x]b", Extended | Regex::Newline, "a\nb"));
    CHECK(Run("^a", Extended, "a", Regex::NotBeginning).status == Regex::Status::NoMatch);
    CHECK(Run("a$", Extended, "a", Regex::NotEnd).status == Regex::Status::NoMatch);
    CHECK(Run("^b", Extended | Regex::Newline, "a\nb", Regex::NotBeginning).spans ==
          Strings{"2-3"});
}

TEST_CASE("regex spans past the subexpressions are -1, and none are needed")
{
    for (const int flags : {Extended, Extended | Regex::NoSubexpressions})
    {
        Regex::Program* program = nullptr;
        size_t subexpressions = 0;
        REQUIRE(Regex::Compile("(a)", flags, &program, &subexpressions) ==
                Regex::Status::Matched);
        CHECK(subexpressions == 1);
        std::vector<Regex::Span> spans(4, Regex::Span{7, 7});
        CHECK(Regex::Execute(*program, "xa", 2, 0, spans.data(), spans.size()) ==
              Regex::Status::Matched);
        Regex::Free(program);
        if (flags == Extended)
        {
            CHECK((spans[0].start == 1 && spans[0].end == 2 && spans[1].start == 1 &&
                   spans[1].end == 2 && spans[2].start == -1 && spans[3].end == -1));
        }
        else
        {
            // REG_NOSUB leaves the spans alone.
            CHECK((spans[0].start == 7 && spans[3].end == 7));
        }
    }
    CHECK(Matches("(a)(b)", Extended, "ab"));
}

TEST_CASE("regex rejects malformed patterns")
{
    CHECK(CompileStatus("[abc", Basic) == Regex::Status::UnmatchedBracket);
    CHECK(CompileStatus("[[:alpha:]", Basic) == Regex::Status::UnmatchedBracket);
    CHECK(CompileStatus("[[:alpha", Basic) == Regex::Status::UnmatchedBracket);
    CHECK(CompileStatus("[[:word:]]", Basic) == Regex::Status::BadClass);
    CHECK(CompileStatus("[[.ab.]]", Basic) == Regex::Status::BadCollatingElement);
    CHECK(CompileStatus("[z-a]", Basic) == Regex::Status::BadRange);
    CHECK(CompileStatus("[[:alpha:]-z]", Basic) == Regex::Status::BadRange);
    CHECK(CompileStatus("(ab", Extended) == Regex::Status::UnmatchedParenthesis);
    CHECK(CompileStatus("\\(ab", Basic) == Regex::Status::UnmatchedParenthesis);
    CHECK(CompileStatus("ab\\)", Basic) == Regex::Status::UnmatchedParenthesis);
    CHECK(CompileStatus("ab\\", Basic) == Regex::Status::BadEscape);
    CHECK(CompileStatus("\\d", Extended) == Regex::Status::BadEscape);
    CHECK(CompileStatus("a\\|b", Basic) == Regex::Status::BadEscape);
    CHECK(CompileStatus("*a", Extended) == Regex::Status::BadRepetition);
    CHECK(CompileStatus("a|+b", Extended) == Regex::Status::BadRepetition);
    CHECK(CompileStatus("(?a)", Extended) == Regex::Status::BadRepetition);
    CHECK(CompileStatus("\\{1\\}", Basic) == Regex::Status::BadRepetition);
    CHECK(CompileStatus("a{1", Extended) == Regex::Status::UnmatchedBrace);
    CHECK(CompileStatus("a{", Extended) == Regex::Status::UnmatchedBrace);
    CHECK(CompileStatus("a{x}", Extended) == Regex::Status::BadInterval);
    CHECK(CompileStatus("a{3,2}", Extended) == Regex::Status::BadInterval);
    CHECK(CompileStatus("a{256}", Extended) == Regex::Status::BadInterval);
    CHECK(CompileStatus("a\\{1,2", Basic) == Regex::Status::UnmatchedBrace);
    CHECK(CompileStatus("\\1(a)", Extended) == Regex::Status::BadReference);
    CHECK(CompileStatus("\\(a\\1\\)", Basic) == Regex::Status::BadReference);
    CHECK(CompileStatus("((a{255}){255}){255}", Extended) == Regex::Status::NoMemory);
    CHECK(std::string(Regex::Describe(Regex::Status::UnmatchedBracket)) ==
          "Unmatched [, [^, [:, [., or [=");
}
