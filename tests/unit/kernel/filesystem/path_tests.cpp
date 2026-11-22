#include <catch2/catch_test_macros.hpp>

#include "kernel/filesystem/path.h"

#include <cstring>
#include <string>

namespace
{

std::string Join(const char* working_directory, const char* path)
{
    char result[FeatherOS::Path::MaximumLength];
    REQUIRE(FeatherOS::Path::Join(working_directory, path, result));
    return result;
}

} // namespace

TEST_CASE("paths join the working directory")
{
    REQUIRE(Join("/home", "notes.txt") == "/home/notes.txt");
    REQUIRE(Join("/home/docs", "old/a.txt") == "/home/docs/old/a.txt");
    REQUIRE(Join("/home/docs", "/hello.txt") == "/hello.txt");
    REQUIRE(Join("/home", "My Notes.txt") == "/home/My Notes.txt");
    REQUIRE(Join("/", "home") == "/home");
}

TEST_CASE("leading dots apply to the working directory, the rest is left for the walk")
{
    REQUIRE(Join("/home/docs", "..") == "/home");
    REQUIRE(Join("/home/docs", "../..") == "/");
    REQUIRE(Join("/home", "../../..") == "/");
    REQUIRE(Join("/home", ".") == "/home");
    REQUIRE(Join("/home", "") == "/home");
    REQUIRE(Join("/home", "./../tmp") == "/tmp");
    REQUIRE(Join("/home", "...") == "/home/...");
    REQUIRE(Join("/home", "..x") == "/home/..x");
    // Past the first name, ".." may follow a symbolic link, so only the walk can apply it.
    REQUIRE(Join("/home", "./a/./b/../c") == "/home/a/./b/../c");
    REQUIRE(Join("/home", "link/..") == "/home/link/..");
    // An absolute path stays as given, for the walk.
    REQUIRE(Join("/home", "//tmp/../etc/") == "//tmp/../etc/");
}

TEST_CASE("paths that would not fit are rejected")
{
    char result[FeatherOS::Path::MaximumLength];
    // "/home/" and 1017 characters make the longest path, PATH_MAX less its terminator.
    static_assert(FeatherOS::Path::MaximumLength == 1024);
    const std::string component(200, 'x');
    const std::string long_name = component + "/" + component + "/" + component + "/" + component +
                                  "/" + std::string(213, 'x');
    REQUIRE(long_name.size() == 1017);
    REQUIRE(FeatherOS::Path::Join("/home", long_name.c_str(), result));
    REQUIRE(std::string(result).size() == 1023);
    REQUIRE_FALSE(FeatherOS::Path::Join("/home", (long_name + "y").c_str(), result));
    // The joined text must fit, before the walk could take a ".." away again.
    REQUIRE_FALSE(FeatherOS::Path::Join("/home", (long_name + "/..").c_str(), result));
    // Leading ".." shortens the working directory first, so this one fits.
    REQUIRE(FeatherOS::Path::Join("/home", ("../" + long_name + "yy").c_str(), result));
    REQUIRE(std::string(result).size() == 1020);
    // Five 200-byte components, as the ext2 test volume nests them.
    std::string nested;
    for (int level = 0; level < 5; ++level)
    {
        nested += "/" + component;
    }
    REQUIRE(FeatherOS::Path::Join("/", nested.c_str(), result));
    REQUIRE(std::string(result) == nested);
    REQUIRE(FeatherOS::Path::Join("/", nested.c_str() + 1, result));
    REQUIRE(std::string(result) == nested);
    REQUIRE_FALSE(
        FeatherOS::Path::Join("/mnt/a/b/c/d/e/f/g/h/i/j/k", nested.c_str() + 1, result));
    std::string absolute(FeatherOS::Path::MaximumLength, 'a');
    absolute[0] = '/';
    REQUIRE_FALSE(FeatherOS::Path::Join("/", absolute.c_str(), result));
    absolute.pop_back();
    REQUIRE(FeatherOS::Path::Join("/", absolute.c_str(), result));
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
