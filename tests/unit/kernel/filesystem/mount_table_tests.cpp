#include <catch2/catch_test_macros.hpp>

#include "kernel/filesystem/filesystem.h"
#include "kernel/filesystem/mount_table.h"

#include <string>
#include <vector>

namespace
{

using FeatherOS::Filesystem::Driver;
using FeatherOS::VFS::MountTable;

class FakeDriver final : public Driver
{
public:
    FakeDriver() : Driver({}) {}
};

std::string Inner(const MountTable& table, const char* path, Driver* expected)
{
    const char* inner = nullptr;
    REQUIRE(table.Resolve(path, &inner) == expected);
    return inner;
}

std::vector<std::string> Children(const MountTable& table, const char* directory)
{
    std::vector<std::string> names;
    FeatherOS::Filesystem::DirectoryEntry entry{};
    for (uint32_t index = 0; table.ChildAt(directory, index, &entry); ++index)
    {
        REQUIRE(entry.directory);
        names.emplace_back(entry.name);
    }
    REQUIRE(names.size() == table.ChildCount(directory));
    return names;
}

} // namespace

TEST_CASE("the mount table picks the longest mount point on whole components")
{
    FakeDriver root;
    FakeDriver tmp;
    FakeDriver nested;
    MountTable table;
    REQUIRE(table.Add("/", &root));
    REQUIRE(table.Add("/tmp", &tmp));
    REQUIRE(table.Add("/tmp/cache", &nested));

    REQUIRE(Inner(table, "/", &root).empty());
    REQUIRE(Inner(table, "/home/docs/a.txt", &root) == "home/docs/a.txt");
    REQUIRE(Inner(table, "/tmp", &tmp).empty());
    REQUIRE(Inner(table, "/tmp/", &tmp).empty());
    REQUIRE(Inner(table, "/tmp/notes.txt", &tmp) == "notes.txt");
    REQUIRE(Inner(table, "/tmp//notes.txt", &tmp) == "notes.txt");
    REQUIRE(Inner(table, "/tmpx", &root) == "tmpx");
    REQUIRE(Inner(table, "/tmpx/a", &root) == "tmpx/a");
    REQUIRE(Inner(table, "/tmp/cache", &nested).empty());
    REQUIRE(Inner(table, "/tmp/cache/a/b", &nested) == "a/b");
    REQUIRE(Inner(table, "/tmp/cached", &tmp) == "cached");
}

TEST_CASE("the mount table resolves nothing without a matching mount")
{
    FakeDriver tmp;
    MountTable table;
    const char* inner = nullptr;
    REQUIRE(table.Resolve("/tmp", &inner) == nullptr);
    REQUIRE(table.Add("/tmp", &tmp));
    REQUIRE(table.Resolve("/", &inner) == nullptr);
    REQUIRE(table.Resolve("/home", &inner) == nullptr);
    REQUIRE(table.Resolve("tmp", &inner) == nullptr);
    REQUIRE(Inner(table, "/tmp/a", &tmp) == "a");
}

TEST_CASE("the mount table refuses malformed, taken and excess mount points")
{
    FakeDriver driver;
    MountTable table;
    REQUIRE_FALSE(table.Add("tmp", &driver));
    REQUIRE_FALSE(table.Add("", &driver));
    REQUIRE_FALSE(table.Add("/tmp/", &driver));
    REQUIRE_FALSE(table.Add("//tmp", &driver));
    REQUIRE_FALSE(table.Add("/a/./b", &driver));
    REQUIRE_FALSE(table.Add("/a/..", &driver));
    REQUIRE_FALSE(table.Add("/tmp", nullptr));
    REQUIRE(table.Add("/tmp", &driver));
    REQUIRE_FALSE(table.Add("/tmp", &driver));
    REQUIRE(table.Find("/tmp") == &driver);
    REQUIRE(table.Find("/tm") == nullptr);

    for (uint32_t index = 1; index < MountTable::Capacity; ++index)
    {
        const std::string point = "/m" + std::to_string(index);
        REQUIRE(table.Add(point.c_str(), &driver));
    }
    REQUIRE_FALSE(table.Add("/full", &driver));
    REQUIRE(table.Remove("/m3"));
    REQUIRE_FALSE(table.Remove("/m3"));
    REQUIRE(table.Find("/m3") == nullptr);
    REQUIRE(table.Add("/full", &driver));
}

TEST_CASE("the mount table lists the mount points directly inside a directory in mount order")
{
    FakeDriver driver;
    MountTable table;
    REQUIRE(table.Add("/", &driver));
    REQUIRE(table.Add("/system", &driver));
    REQUIRE(table.Add("/etc", &driver));
    REQUIRE(table.Add("/mnt/usb", &driver));
    REQUIRE(table.Add("/dev", &driver));
    REQUIRE(table.Add("/mnt/usb/part1", &driver));

    REQUIRE(Children(table, "/") == std::vector<std::string>{"system", "etc", "dev"});
    REQUIRE(Children(table, "/mnt") == std::vector<std::string>{"usb"});
    REQUIRE(Children(table, "/mnt/") == std::vector<std::string>{"usb"});
    REQUIRE(Children(table, "/mnt/usb") == std::vector<std::string>{"part1"});
    REQUIRE(Children(table, "/mn").empty());
    REQUIRE(Children(table, "/etc").empty());
    REQUIRE(Children(table, "system").empty());

    REQUIRE(table.Remove("/etc"));
    REQUIRE(Children(table, "/") == std::vector<std::string>{"system", "dev"});
}
