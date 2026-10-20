#include <catch2/catch_test_macros.hpp>

#include "kernel/filesystem/path.h"

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
    REQUIRE(Resolve("/disk", "notes.txt") == "/disk/notes.txt");
    REQUIRE(Resolve("/disk/docs", "old/a.txt") == "/disk/docs/old/a.txt");
    REQUIRE(Resolve("/disk/docs", "/hello.txt") == "/hello.txt");
    REQUIRE(Resolve("/disk", "My Notes.txt") == "/disk/My Notes.txt");
}

TEST_CASE("paths collapse dots and repeated slashes")
{
    REQUIRE(Resolve("/disk/docs", "..") == "/disk");
    REQUIRE(Resolve("/disk/docs", "../..") == "/");
    REQUIRE(Resolve("/disk", "../../..") == "/");
    REQUIRE(Resolve("/disk", "./a/./b/../c") == "/disk/a/c");
    REQUIRE(Resolve("/", "//disk///docs//") == "/disk/docs");
    REQUIRE(Resolve("/disk", ".") == "/disk");
    REQUIRE(Resolve("/disk", "") == "/disk");
    REQUIRE(Resolve("/disk", "...") == "/disk/...");
}

TEST_CASE("paths that would not fit are rejected")
{
    char result[FeatherOS::Path::MaximumLength];
    // "/disk/" and 57 characters make the longest path, 63 characters.
    const std::string long_name(57, 'x');
    REQUIRE(FeatherOS::Path::Resolve("/disk", long_name.c_str(), result));
    REQUIRE(std::string(result).size() == 63);
    REQUIRE_FALSE(FeatherOS::Path::Resolve("/disk", (long_name + "y").c_str(), result));
    // Components collapse as they are read, so an intermediate path must fit too.
    REQUIRE_FALSE(FeatherOS::Path::Resolve("/disk", (long_name + "y/..").c_str(), result));
}
