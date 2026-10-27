#include <catch2/catch_test_macros.hpp>

#include "kernel/filesystem/path.h"

#include <cstring>
#include <string>

namespace
{

std::string Resolve(const char* working_directory, const char* path)
{
    char result[FeatherOS::Path::MaximumLength];
    REQUIRE(FeatherOS::Path::Resolve(working_directory, path, result));
    return result;
}

} // namespace

TEST_CASE("paths resolve against the working directory")
{
    REQUIRE(Resolve("/home", "notes.txt") == "/home/notes.txt");
    REQUIRE(Resolve("/home/docs", "old/a.txt") == "/home/docs/old/a.txt");
    REQUIRE(Resolve("/home/docs", "/hello.txt") == "/hello.txt");
    REQUIRE(Resolve("/home", "My Notes.txt") == "/home/My Notes.txt");
}

TEST_CASE("paths collapse dots and repeated slashes")
{
    REQUIRE(Resolve("/home/docs", "..") == "/home");
    REQUIRE(Resolve("/home/docs", "../..") == "/");
    REQUIRE(Resolve("/home", "../../..") == "/");
    REQUIRE(Resolve("/home", "./a/./b/../c") == "/home/a/c");
    REQUIRE(Resolve("/", "//home///docs//") == "/home/docs");
    REQUIRE(Resolve("/home", ".") == "/home");
    REQUIRE(Resolve("/home", "") == "/home");
    REQUIRE(Resolve("/home", "...") == "/home/...");
}

TEST_CASE("paths that would not fit are rejected")
{
    char result[FeatherOS::Path::MaximumLength];
    // "/home/" and 57 characters make the longest path, 63 characters.
    const std::string long_name(57, 'x');
    REQUIRE(FeatherOS::Path::Resolve("/home", long_name.c_str(), result));
    REQUIRE(std::string(result).size() == 63);
    REQUIRE_FALSE(FeatherOS::Path::Resolve("/home", (long_name + "y").c_str(), result));
    // Components collapse as they are read, so an intermediate path must fit too.
    REQUIRE_FALSE(FeatherOS::Path::Resolve("/home", (long_name + "y/..").c_str(), result));
}

TEST_CASE("/proc/self names the calling process")
{
    auto replace = [](const char* text, uint64_t process_id)
    {
        char path[FeatherOS::Path::MaximumLength];
        std::strcpy(path, text);
        REQUIRE(FeatherOS::Path::ReplaceProcSelf(path, process_id));
        return std::string(path);
    };
    REQUIRE(replace("/proc/self", 7) == "/proc/7");
    REQUIRE(replace("/proc/self/status", 7) == "/proc/7/status");
    REQUIRE(replace("/proc/self/status", 123456) == "/proc/123456/status");
    REQUIRE(replace("/proc/selfish", 7) == "/proc/selfish");
    REQUIRE(replace("/home/proc/self", 7) == "/home/proc/self");
    REQUIRE(replace("/proc/meminfo", 7) == "/proc/meminfo");

    char path[FeatherOS::Path::MaximumLength] = "/proc/self/";
    std::memset(path + 11, 'x', sizeof(path) - 12);
    path[sizeof(path) - 1] = '\0';
    REQUIRE_FALSE(FeatherOS::Path::ReplaceProcSelf(path, 123456));
}

