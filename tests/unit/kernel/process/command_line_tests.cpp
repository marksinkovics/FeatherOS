#include <catch2/catch_test_macros.hpp>

#include "kernel/process_arguments.h"
#include "libc/spawn_request.h"
#include "user/command_line.h"

#include <cstddef>
#include <string>
#include <vector>

namespace
{

using FeatherOS::User::SplitCommandLine;
using FeatherOS::User::SplitResult;

constexpr uint64_t UserAddress = 0x0000400000802000;

std::vector<std::string> Split(const std::string& line, SplitResult expected = SplitResult::Done)
{
    std::vector<char> text(line.begin(), line.end());
    text.push_back('\0');
    char* words[FeatherOS::User::MaximumArgumentCount + 1];
    size_t count = 0;
    REQUIRE(SplitCommandLine(text.data(), words, sizeof(words) / sizeof(words[0]), &count) ==
            expected);
    std::vector<std::string> result;
    if (expected == SplitResult::Done)
    {
        REQUIRE(words[count] == nullptr);
        for (size_t index = 0; index < count; ++index)
        {
            result.emplace_back(words[index]);
        }
    }
    return result;
}

// What a program started with this command line receives as its arguments.
std::vector<std::string> KernelSplit(const char* line)
{
    FeatherOS::User::ProgramArguments arguments{};
    REQUIRE(FeatherOS::Process::BuildProgramArguments(line, UserAddress, &arguments));
    const uint64_t storage =
        UserAddress + offsetof(FeatherOS::User::ProgramArguments, argument_storage);
    std::vector<std::string> result;
    for (uint64_t index = 0; index < arguments.argument_count; ++index)
    {
        const uint64_t address = reinterpret_cast<uint64_t>(arguments.argument_values[index]);
        result.emplace_back(arguments.argument_storage + (address - storage));
    }
    return result;
}

} // namespace

TEST_CASE("the shell splits a line into words at spaces")
{
    REQUIRE(Split("echo hello   feather os").size() == 4);
    REQUIRE(Split("  ls  ") == std::vector<std::string>{"ls"});
    REQUIRE(Split("").empty());
    REQUIRE(Split("   ").empty());
}

TEST_CASE("double quotes group words and are dropped")
{
    REQUIRE(Split("cat \"my plans.txt\"") == std::vector<std::string>{"cat", "my plans.txt"});
    REQUIRE(Split("echo a\"b c\"d \"\"") == std::vector<std::string>{"echo", "ab cd", ""});
    Split("cat \"open", SplitResult::UnclosedQuote);
}

TEST_CASE("the shell refuses more words than the kernel accepts")
{
    std::string line = "echo";
    for (int index = 1; index < 16; ++index)
    {
        line += " w";
    }
    REQUIRE(Split(line).size() == 16);
    Split(line + " w", SplitResult::TooManyWords);
}

// The shell splits a line, posix_spawn joins the words into a command line again, and the kernel
// splits that: the program must get the words the shell saw.
TEST_CASE("words survive the trip through posix_spawn and the kernel")
{
    for (const char* line : {"echo hello   feather os", "cat \"my plans.txt\"", "echo \"\" x",
                             "write -a \"a b\"c d", "calc 4 + 66 - (3 / 4)"})
    {
        INFO(line);
        const std::vector<std::string> words = Split(line);
        std::vector<char*> argv;
        for (const std::string& word : words)
        {
            argv.push_back(const_cast<char*>(word.c_str()));
        }
        argv.push_back(nullptr);
        char command_line[FeatherOS::User::MaximumCommandLineLength];
        REQUIRE(FeatherOS::LibC::BuildCommandLine(argv[0], argv.data(), command_line) ==
                FeatherOS::LibC::SpawnRequestResult::Done);
        REQUIRE(KernelSplit(command_line) == words);
    }
}

TEST_CASE("help is the only argument -h or --help")
{
    char name[] = "ls";
    char short_flag[] = "-h";
    char long_flag[] = "--help";
    char other[] = "docs";
    char* with_short[] = {name, short_flag};
    char* with_long[] = {name, long_flag};
    char* with_other[] = {name, other};
    char* with_two[] = {name, short_flag, other};
    REQUIRE(FeatherOS::User::HelpRequested(2, with_short));
    REQUIRE(FeatherOS::User::HelpRequested(2, with_long));
    REQUIRE_FALSE(FeatherOS::User::HelpRequested(2, with_other));
    REQUIRE_FALSE(FeatherOS::User::HelpRequested(3, with_two));
    REQUIRE_FALSE(FeatherOS::User::HelpRequested(1, with_short));
}

TEST_CASE("only whole decimal numbers parse")
{
    unsigned long long value = 0;
    REQUIRE(FeatherOS::User::ParseUnsigned("1100", &value));
    REQUIRE(value == 1100);
    REQUIRE(FeatherOS::User::ParseUnsigned("18446744073709551615", &value));
    REQUIRE(value == ~0ULL);
    for (const char* text : {"", "-1", "+1", " 1", "1 ", "12a", "18446744073709551616"})
    {
        INFO(text);
        REQUIRE_FALSE(FeatherOS::User::ParseUnsigned(text, &value));
    }
}

TEST_CASE("redirections leave the words and keep their order")
{
    char line[] = "sort < in.txt >out.txt 2>&1 extra \">\" 2> errors";
    char* words[FeatherOS::User::MaximumArgumentCount + 1];
    bool quoted[FeatherOS::User::MaximumArgumentCount];
    size_t count = 0;
    REQUIRE(SplitCommandLine(line, words, FeatherOS::User::MaximumArgumentCount + 1, &count,
                             quoted) == SplitResult::Done);
    FeatherOS::User::Redirect redirects[4];
    size_t redirect_count = 0;
    REQUIRE(FeatherOS::User::ExtractRedirects(words, quoted, &count, redirects, 4,
                                              &redirect_count) ==
            FeatherOS::User::RedirectResult::Done);
    REQUIRE(count == 3);
    REQUIRE(std::string(words[0]) == "sort");
    REQUIRE(std::string(words[1]) == "extra");
    REQUIRE(std::string(words[2]) == ">");
    REQUIRE(words[3] == nullptr);
    REQUIRE(redirect_count == 4);
    REQUIRE(redirects[0].kind == FeatherOS::User::RedirectKind::Input);
    REQUIRE(std::string(redirects[0].path) == "in.txt");
    REQUIRE(redirects[1].kind == FeatherOS::User::RedirectKind::Output);
    REQUIRE(std::string(redirects[1].path) == "out.txt");
    REQUIRE(redirects[2].kind == FeatherOS::User::RedirectKind::ErrorToOutput);
    REQUIRE(redirects[2].path == nullptr);
    REQUIRE(redirects[3].kind == FeatherOS::User::RedirectKind::Error);
    REQUIRE(std::string(redirects[3].path) == "errors");
}

TEST_CASE("a redirection without a file, or too many, is refused")
{
    auto extract = [](std::string text, size_t capacity)
    {
        std::vector<char> line(text.begin(), text.end());
        line.push_back('\0');
        char* words[FeatherOS::User::MaximumArgumentCount + 1];
        bool quoted[FeatherOS::User::MaximumArgumentCount];
        size_t count = 0;
        REQUIRE(SplitCommandLine(line.data(), words, FeatherOS::User::MaximumArgumentCount + 1,
                                 &count, quoted) == SplitResult::Done);
        FeatherOS::User::Redirect redirects[4];
        size_t redirect_count = 0;
        return FeatherOS::User::ExtractRedirects(words, quoted, &count, redirects, capacity,
                                                 &redirect_count);
    };
    REQUIRE(extract("echo hi >", 4) == FeatherOS::User::RedirectResult::MissingPath);
    REQUIRE(extract("echo hi >>a >>b", 1) == FeatherOS::User::RedirectResult::TooMany);
    REQUIRE(extract("echo \"a > b\"", 4) == FeatherOS::User::RedirectResult::Done);
}

TEST_CASE("unquoted pipes get spaces around them")
{
    char output[64];
    REQUIRE(FeatherOS::User::SeparatePipes("ls|upper", output, sizeof(output)));
    REQUIRE(std::string(output) == "ls | upper");
    REQUIRE(FeatherOS::User::SeparatePipes("echo \"a|b\" |cat", output, sizeof(output)));
    REQUIRE(std::string(output) == "echo \"a|b\"  | cat");
    REQUIRE_FALSE(FeatherOS::User::SeparatePipes("a|b", output, 5));
}

TEST_CASE("a pipeline splits into stages at unquoted bars")
{
    char line[] = "cat notes.txt | upper > out.txt | echo \"|\"";
    char* words[FeatherOS::User::MaximumArgumentCount + 1];
    bool quoted[FeatherOS::User::MaximumArgumentCount];
    size_t count = 0;
    REQUIRE(SplitCommandLine(line, words, FeatherOS::User::MaximumArgumentCount + 1, &count,
                             quoted) == SplitResult::Done);
    FeatherOS::User::Stage stages[4];
    size_t stage_count = 0;
    REQUIRE(FeatherOS::User::SplitPipeline(words, quoted, count, stages, 4, &stage_count) ==
            FeatherOS::User::PipelineResult::Done);
    REQUIRE(stage_count == 3);
    REQUIRE(stages[0].count == 2);
    REQUIRE(std::string(stages[0].words[1]) == "notes.txt");
    REQUIRE(stages[0].words[2] == nullptr);
    REQUIRE(stages[1].count == 3);
    REQUIRE(std::string(stages[1].words[0]) == "upper");
    REQUIRE(stages[2].count == 2);
    REQUIRE(std::string(stages[2].words[1]) == "|");
    REQUIRE(stages[2].quoted[1]);

    auto split = [](std::string text, size_t capacity)
    {
        std::vector<char> buffer(text.begin(), text.end());
        buffer.push_back('\0');
        char* parts[FeatherOS::User::MaximumArgumentCount + 1];
        bool marks[FeatherOS::User::MaximumArgumentCount];
        size_t total = 0;
        REQUIRE(SplitCommandLine(buffer.data(), parts, FeatherOS::User::MaximumArgumentCount + 1,
                                 &total, marks) == SplitResult::Done);
        FeatherOS::User::Stage found[4];
        size_t found_count = 0;
        return FeatherOS::User::SplitPipeline(parts, marks, total, found, capacity, &found_count);
    };
    REQUIRE(split("ls | | cat", 4) == FeatherOS::User::PipelineResult::EmptyStage);
    REQUIRE(split("ls |", 4) == FeatherOS::User::PipelineResult::EmptyStage);
    REQUIRE(split("a | b | c", 2) == FeatherOS::User::PipelineResult::TooManyStages);
}

TEST_CASE("a trailing & is taken off and runs the command in the background")
{
    auto take = [](std::string text, std::string* rest)
    {
        std::vector<char> line(text.begin(), text.end());
        line.push_back('\0');
        char* words[FeatherOS::User::MaximumArgumentCount + 1];
        bool quoted[FeatherOS::User::MaximumArgumentCount];
        size_t count = 0;
        REQUIRE(SplitCommandLine(line.data(), words, FeatherOS::User::MaximumArgumentCount + 1,
                                 &count, quoted) == SplitResult::Done);
        const bool background = FeatherOS::User::TakeBackgroundMarker(words, quoted, &count);
        rest->clear();
        for (size_t index = 0; index < count; ++index)
        {
            *rest += (index > 0 ? " " : "") + std::string(words[index]);
        }
        REQUIRE(words[count] == nullptr);
        return background;
    };
    std::string rest;
    REQUIRE(take("sleep 300 &", &rest));
    REQUIRE(rest == "sleep 300");
    REQUIRE(take("sleep 300&", &rest));
    REQUIRE(rest == "sleep 300");
    REQUIRE_FALSE(take("echo \"&\"", &rest));
    REQUIRE(rest == "echo &");
    REQUIRE_FALSE(take("echo a&b", &rest));
    REQUIRE_FALSE(take("", &rest));
}

