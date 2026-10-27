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

std::vector<std::string> Candidates(const char* search, const char* file)
{
    std::vector<std::string> candidates;
    char candidate[FeatherOS::User::MaximumPathLength];
    while (FeatherOS::LibC::NextSearchCandidate(&search, file, candidate))
    {
        candidates.emplace_back(candidate);
    }
    return candidates;
}

} // namespace

TEST_CASE("spawn requests shorten /system paths and quote words with spaces")
{
    std::string line;
    REQUIRE(Build("/system/ls", {"ls", "/home"}, &line) == SpawnRequestResult::Done);
    REQUIRE(line == "ls /home");
    REQUIRE(Build("cat", {"ignored", "my notes.txt", "", "x"}, &line) == SpawnRequestResult::Done);
    REQUIRE(line == "cat \"my notes.txt\" \"\" x");
    REQUIRE(Build("echo", {"echo"}, &line) == SpawnRequestResult::Done);
    REQUIRE(line == "echo");
}

TEST_CASE("spawn requests pass other paths on for the kernel to resolve")
{
    std::string line;
    REQUIRE(Build("/bin/tool", {"tool", "a"}, &line) == SpawnRequestResult::Done);
    REQUIRE(line == "/bin/tool a");
    REQUIRE(Build("./ls", {"ls"}, &line) == SpawnRequestResult::Done);
    REQUIRE(line == "./ls");
    REQUIRE(Build("/system/", {"x"}, &line) == SpawnRequestResult::Done);
    REQUIRE(line == "/system/");
    REQUIRE(Build("/system/sub/tool", {"tool"}, &line) == SpawnRequestResult::Done);
    REQUIRE(line == "/system/sub/tool");
    REQUIRE(Build("/tmp/my tool", {"tool", "x"}, &line) == SpawnRequestResult::Done);
    REQUIRE(line == "\"/tmp/my tool\" x");
}

TEST_CASE("spawn requests reject paths and arguments the kernel cannot run")
{
    std::string line;
    REQUIRE(Build("", {"x"}, &line) == SpawnRequestResult::NotFound);
    REQUIRE(Build("/tmp/\"x\"", {"x"}, &line) == SpawnRequestResult::Invalid);
    REQUIRE(Build(("/" + std::string(70, 'p')).c_str(), {"x"}, &line) ==
            SpawnRequestResult::TooBig);
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

TEST_CASE("PATH searches join each directory to the file")
{
    REQUIRE(Candidates("/system:/bin", "ls") == std::vector<std::string>{"/system/ls", "/bin/ls"});
    REQUIRE(Candidates("::/bin/:", "ls") == std::vector<std::string>{"/bin/ls"});
    REQUIRE(Candidates("", "ls").empty());
    // A candidate must fit in a path, terminator included.
    const std::string long_directory = "/" + std::string(60, 'd');
    const std::string search = long_directory + ":/" + std::string(59, 'e') + ":/bin";
    REQUIRE(Candidates(search.c_str(), "ls") ==
            std::vector<std::string>{"/" + std::string(59, 'e') + "/ls", "/bin/ls"});
}
