#include <catch2/catch_test_macros.hpp>

#include "libc/environment_table.h"

#include <string>
#include <vector>

namespace
{

using FeatherOS::LibC::EnvironmentResult;
using FeatherOS::LibC::EnvironmentTable;

std::vector<std::string> Entries(EnvironmentTable& table)
{
    std::vector<std::string> entries;
    for (char** entry = table.Entries(); *entry != nullptr; ++entry)
    {
        entries.emplace_back(*entry);
    }
    return entries;
}

// The block in Spawn's format, without its final empty string.
std::string Block(const EnvironmentTable& table)
{
    const char* block = table.Block();
    std::string text;
    while (*block != '\0')
    {
        const std::string entry(block);
        text += entry + '|';
        block += entry.size() + 1;
    }
    return text;
}

} // namespace

TEST_CASE("the environment loads the block the kernel passes")
{
    EnvironmentTable table;
    table.Load("HOME=/home\0PATH=/system\0");
    REQUIRE(Entries(table) == std::vector<std::string>{"HOME=/home", "PATH=/system"});
    REQUIRE(std::string(table.Find("HOME")) == "/home");
    REQUIRE(table.Find("HOM") == nullptr);
    REQUIRE(table.Find("HOME=") == nullptr);
    REQUIRE(table.Find("MISSING") == nullptr);
    table.Load(nullptr);
    REQUIRE(Entries(table).empty());
    REQUIRE(*table.Block() == '\0');
}

TEST_CASE("setting a variable appends it or replaces the old value")
{
    EnvironmentTable table;
    table.Load("A=1\0B=2\0C=3\0");
    REQUIRE(table.Set("D", "4", false) == EnvironmentResult::Done);
    REQUIRE(table.Set("B", "kept", false) == EnvironmentResult::Done);
    REQUIRE(std::string(table.Find("B")) == "2");
    REQUIRE(table.Set("B", "twenty", true) == EnvironmentResult::Done);
    REQUIRE(Entries(table) == std::vector<std::string>{"A=1", "C=3", "D=4", "B=twenty"});
    REQUIRE(Block(table) == "A=1|C=3|D=4|B=twenty|");
    REQUIRE(table.Set("C", table.Find("C"), true) == EnvironmentResult::Done);
    REQUIRE(std::string(table.Find("C")) == "3");
    REQUIRE(table.Set("E", "", true) == EnvironmentResult::Done);
    REQUIRE(std::string(table.Find("E")).empty());
}

TEST_CASE("removing a variable closes the gap it leaves")
{
    EnvironmentTable table;
    table.Load("A=1\0BB=22\0C=3\0");
    REQUIRE(table.Remove("BB") == EnvironmentResult::Done);
    REQUIRE(Entries(table) == std::vector<std::string>{"A=1", "C=3"});
    REQUIRE(Block(table) == "A=1|C=3|");
    REQUIRE(table.Remove("MISSING") == EnvironmentResult::Done);
    REQUIRE(table.Remove("A") == EnvironmentResult::Done);
    REQUIRE(table.Remove("C") == EnvironmentResult::Done);
    REQUIRE(Entries(table).empty());
}

TEST_CASE("variable names must be non-empty and contain no '='")
{
    EnvironmentTable table;
    table.Load(nullptr);
    REQUIRE(table.Set("", "x", true) == EnvironmentResult::InvalidName);
    REQUIRE(table.Set("A=B", "x", true) == EnvironmentResult::InvalidName);
    REQUIRE(table.Set(nullptr, "x", true) == EnvironmentResult::InvalidName);
    REQUIRE(table.Remove("A=B") == EnvironmentResult::InvalidName);
}

TEST_CASE("the environment is limited to what Spawn can pass on")
{
    EnvironmentTable table;
    table.Load(nullptr);
    const std::string too_long(FeatherOS::User::MaximumEnvironmentSize, 'x');
    REQUIRE(table.Set("LONG", too_long.c_str(), true) == EnvironmentResult::Full);
    // The largest entry that fits: "L=" plus the value, its terminator, and the block's own.
    const std::string longest(FeatherOS::User::MaximumEnvironmentSize - 4, 'x');
    REQUIRE(table.Set("L", longest.c_str(), true) == EnvironmentResult::Done);
    REQUIRE(table.Set("M", "", true) == EnvironmentResult::Full);
    // Replacing the large entry frees its space first.
    REQUIRE(table.Set("L", "short", true) == EnvironmentResult::Done);

    table.Load(nullptr);
    for (uint64_t index = 0; index < FeatherOS::User::MaximumEnvironmentVariables; ++index)
    {
        REQUIRE(table.Set(("V" + std::to_string(index)).c_str(), "1", true) ==
                EnvironmentResult::Done);
    }
    REQUIRE(table.Set("ONE_MORE", "1", true) == EnvironmentResult::Full);
    REQUIRE(table.Set("V0", "2", true) == EnvironmentResult::Done);
}
