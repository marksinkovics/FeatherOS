#include <catch2/catch_test_macros.hpp>

#include "libc/spawn_request.h"

#include <initializer_list>
#include <string>
#include <vector>

namespace
{

using FeatherOS::LibC::SpawnRequestResult;

SpawnRequestResult Build(const char* path, std::initializer_list<const char*> arguments,
                         std::string* line)
{
    std::vector<char*> argv;
    for (const char* argument : arguments)
    {
        argv.push_back(const_cast<char*>(argument));
    }
    argv.push_back(nullptr);
    char text[FeatherOS::User::MaximumCommandLineLength];
    const SpawnRequestResult result = FeatherOS::LibC::BuildCommandLine(path, argv.data(), text);
    *line = text;
    return result;
}

} // namespace

TEST_CASE("spawn requests name a program in /system and quote arguments with spaces")
{
    std::string line;
    REQUIRE(Build("/system/ls", {"ls", "/home"}, &line) == SpawnRequestResult::Done);
    REQUIRE(line == "ls /home");
    REQUIRE(Build("cat", {"ignored", "my notes.txt", "", "x"}, &line) == SpawnRequestResult::Done);
    REQUIRE(line == "cat \"my notes.txt\" \"\" x");
    REQUIRE(Build("echo", {"echo"}, &line) == SpawnRequestResult::Done);
    REQUIRE(line == "echo");
}

TEST_CASE("spawn requests reject paths and arguments the kernel cannot run")
{
    std::string line;
    REQUIRE(Build("/home/tool", {"tool"}, &line) == SpawnRequestResult::NotFound);
    REQUIRE(Build("/system/", {"x"}, &line) == SpawnRequestResult::NotFound);
    REQUIRE(Build("./ls", {"ls"}, &line) == SpawnRequestResult::NotFound);
    REQUIRE(Build("echo", {"echo", "say \"hi\""}, &line) == SpawnRequestResult::Invalid);
    REQUIRE(Build("echo", {"echo", std::string(70, 'x').c_str()}, &line) ==
            SpawnRequestResult::TooBig);
    REQUIRE(Build("echo", {"echo", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11",
                           "12", "13", "14", "15", "16"},
                  &line) == SpawnRequestResult::TooBig);
}

TEST_CASE("spawn environments pack NAME=value strings into one block")
{
    char block[FeatherOS::User::MaximumEnvironmentSize];
    char home[] = "HOME=/home";
    char path[] = "PATH=/system";
    char* variables[] = {home, path, nullptr};
    REQUIRE(FeatherOS::LibC::BuildEnvironmentBlock(variables, block) == SpawnRequestResult::Done);
    REQUIRE(std::string(block, 25) == std::string("HOME=/home\0PATH=/system\0\0", 25));
    REQUIRE(FeatherOS::LibC::BuildEnvironmentBlock(nullptr, block) == SpawnRequestResult::Done);
    REQUIRE(block[0] == '\0');

    char unnamed[] = "=value";
    char* bad[] = {unnamed, nullptr};
    REQUIRE(FeatherOS::LibC::BuildEnvironmentBlock(bad, block) == SpawnRequestResult::Invalid);
    std::string large = "BIG=" + std::string(FeatherOS::User::MaximumEnvironmentSize, 'x');
    char* too_big[] = {large.data(), nullptr};
    REQUIRE(FeatherOS::LibC::BuildEnvironmentBlock(too_big, block) == SpawnRequestResult::TooBig);
}
