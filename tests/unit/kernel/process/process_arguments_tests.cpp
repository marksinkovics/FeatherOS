#include <catch2/catch_test_macros.hpp>

#include "kernel/process_arguments.h"

#include <cstddef>
#include <cstring>
#include <string>

namespace
{

constexpr uint64_t UserAddress = 0x0000400000802000;

// Resolves a user-space argument pointer back into the kernel-side copy of the block.
std::string ArgumentAt(const FeatherOS::User::ProgramArguments& arguments, uint64_t index)
{
    const uint64_t address = reinterpret_cast<uint64_t>(arguments.argument_values[index]);
    const uint64_t storage =
        UserAddress + offsetof(FeatherOS::User::ProgramArguments, argument_storage);
    REQUIRE(address >= storage);
    REQUIRE(address - storage < sizeof(arguments.argument_storage));
    return std::string(arguments.argument_storage + (address - storage));
}

} // namespace

TEST_CASE("program arguments split the command line at spaces")
{
    FeatherOS::User::ProgramArguments arguments;
    std::memset(&arguments, 0xA5, sizeof(arguments));
    REQUIRE(FeatherOS::Process::BuildProgramArguments("  calc 12   30 ", UserAddress, &arguments));

    REQUIRE(std::string(arguments.command_line) == "  calc 12   30 ");
    REQUIRE(arguments.process_id == 0);
    REQUIRE(arguments.argument_count == 3);
    REQUIRE(ArgumentAt(arguments, 0) == "calc");
    REQUIRE(ArgumentAt(arguments, 1) == "12");
    REQUIRE(ArgumentAt(arguments, 2) == "30");
    REQUIRE(arguments.argument_values[3] == nullptr);
}

TEST_CASE("program arguments accept an empty command line")
{
    FeatherOS::User::ProgramArguments arguments;
    REQUIRE(FeatherOS::Process::BuildProgramArguments("", UserAddress, &arguments));
    REQUIRE(arguments.argument_count == 0);
    REQUIRE(arguments.command_line[0] == '\0');
}

TEST_CASE("program arguments fill storage with a maximum-length line")
{
    // 63 characters: 32 one-letter arguments would exceed the count, so use long words.
    const std::string line(FeatherOS::User::MaximumCommandLineLength - 1, 'x');
    FeatherOS::User::ProgramArguments arguments;
    REQUIRE(FeatherOS::Process::BuildProgramArguments(line.c_str(), UserAddress, &arguments));
    REQUIRE(arguments.argument_count == 1);
    REQUIRE(ArgumentAt(arguments, 0) == line);
}

TEST_CASE("program arguments reject oversized lines and argument counts")
{
    FeatherOS::User::ProgramArguments arguments;
    const std::string long_line(FeatherOS::User::MaximumCommandLineLength, 'x');
    REQUIRE_FALSE(
        FeatherOS::Process::BuildProgramArguments(long_line.c_str(), UserAddress, &arguments));
    REQUIRE_FALSE(FeatherOS::Process::BuildProgramArguments("a b c d e f g h i j k l m n o p q",
                                                            UserAddress, &arguments));
    REQUIRE(FeatherOS::Process::BuildProgramArguments("a b c d e f g h i j k l m n o p",
                                                      UserAddress, &arguments));
    REQUIRE(arguments.argument_count == FeatherOS::User::MaximumArgumentCount);
}

TEST_CASE("program arguments group quoted words and drop the quotes")
{
    FeatherOS::User::ProgramArguments arguments;
    REQUIRE(FeatherOS::Process::BuildProgramArguments("cat \"my notes.txt\" a\"b c\"d \"\"",
                                                      UserAddress, &arguments));
    REQUIRE(arguments.argument_count == 4);
    REQUIRE(ArgumentAt(arguments, 1) == "my notes.txt");
    REQUIRE(ArgumentAt(arguments, 2) == "ab cd");
    REQUIRE(ArgumentAt(arguments, 3).empty());
    REQUIRE_FALSE(
        FeatherOS::Process::BuildProgramArguments("cat \"unterminated", UserAddress, &arguments));
}
