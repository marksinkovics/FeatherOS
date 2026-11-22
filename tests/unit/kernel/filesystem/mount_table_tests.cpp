#include <catch2/catch_test_macros.hpp>

#include "kernel/filesystem/filesystem.h"
#include "kernel/filesystem/mount_table.h"
#include "kernel/filesystem/type_registry.h"

#include <array>
#include <string>
#include <vector>

namespace
{

using FeatherOS::Filesystem::Driver;
using FeatherOS::VFS::MountResult;
using FeatherOS::VFS::MountTable;
using FeatherOS::VFS::TypeRegistry;

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

std::vector<std::string> Children(MountTable& table, const char* directory)
{
    std::vector<std::string> names;
    FeatherOS::Filesystem::DirectoryEntry entry{};
    Driver* driver = nullptr;
    for (uint32_t index = 0; table.AcquireChild(directory, index, &entry, &driver); ++index)
    {
        REQUIRE(entry.type == FeatherOS::User::ModeDirectory);
        REQUIRE(driver != nullptr);
        table.Release(driver);
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
    std::array<FakeDriver, MountTable::Capacity + 1> drivers;
    FakeDriver& driver = drivers[0];
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

    REQUIRE_FALSE(table.Add("/elsewhere", &driver));
    REQUIRE(table.Holds(&driver));

    for (uint32_t index = 1; index < MountTable::Capacity; ++index)
    {
        const std::string point = "/m" + std::to_string(index);
        REQUIRE(table.Add(point.c_str(), &drivers[index]));
    }
    REQUIRE(table.Count() == MountTable::Capacity);
    REQUIRE_FALSE(table.Add("/full", &drivers[MountTable::Capacity]));
    REQUIRE(table.Remove("/m3"));
    REQUIRE_FALSE(table.Remove("/m3"));
    REQUIRE(table.Find("/m3") == nullptr);
    REQUIRE_FALSE(table.Holds(&drivers[3]));
    REQUIRE(table.Add("/full", &drivers[MountTable::Capacity]));
}

TEST_CASE("the mount table lists the mount points directly inside a directory in mount order")
{
    std::array<FakeDriver, 6> drivers;
    MountTable table;
    REQUIRE(table.Add("/", &drivers[0]));
    REQUIRE(table.Add("/system", &drivers[1]));
    REQUIRE(table.Add("/etc", &drivers[2]));
    REQUIRE(table.Add("/mnt/usb", &drivers[3]));
    REQUIRE(table.Add("/dev", &drivers[4]));
    REQUIRE(table.Add("/mnt/usb/part1", &drivers[5]));

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

TEST_CASE("the mount table lists every mount in mount order")
{
    FakeDriver root;
    FakeDriver tmp;
    MountTable table;
    REQUIRE(table.Add("/tmp", &tmp));
    REQUIRE(table.Add("/", &root));
    const char* point = nullptr;
    Driver* driver = nullptr;
    REQUIRE(table.At(0, &point, &driver));
    REQUIRE(std::string(point) == "/tmp");
    REQUIRE(driver == &tmp);
    REQUIRE(table.At(1, &point, &driver));
    REQUIRE(std::string(point) == "/");
    REQUIRE(driver == &root);
    REQUIRE_FALSE(table.At(2, &point, &driver));
}

TEST_CASE("unmounting waits for a mount's users and for mount points inside it")
{
    FakeDriver root;
    FakeDriver tmp;
    FakeDriver cache;
    MountTable table;
    REQUIRE(table.Add("/", &root));
    REQUIRE(table.Add("/tmp", &tmp));
    REQUIRE(table.Add("/tmp/cache", &cache));
    Driver* driver = nullptr;

    REQUIRE(table.BeginUnmount("/home", &driver) == MountResult::NotFound);
    REQUIRE(table.BeginUnmount("/", &driver) == MountResult::Busy);
    REQUIRE(table.BeginUnmount("/tmp", &driver) == MountResult::Busy);

    const char* inner = nullptr;
    REQUIRE(table.Acquire("/tmp/cache/a", &inner) == &cache);
    REQUIRE(std::string(inner) == "a");
    REQUIRE(table.BeginUnmount("/tmp/cache", &driver) == MountResult::Busy);
    table.Release(&cache);
    REQUIRE(table.AcquirePoint("/tmp/cache") == &cache);
    REQUIRE(table.BeginUnmount("/tmp/cache", &driver) == MountResult::Busy);
    table.Release(&cache);

    REQUIRE(table.BeginUnmount("/tmp/cache", &driver) == MountResult::Done);
    REQUIRE(driver == &cache);
    // Lookups neither use a mount being unmounted nor fall through to its parent.
    REQUIRE(table.Acquire("/tmp/cache/a", &inner) == nullptr);
    REQUIRE(table.AcquirePoint("/tmp/cache") == nullptr);
    REQUIRE(table.BeginUnmount("/tmp/cache", &driver) == MountResult::Busy);
    REQUIRE(table.Acquire("/tmp/a", &inner) == &tmp);
    table.Release(&tmp);

    table.CancelUnmount("/tmp/cache");
    REQUIRE(table.Acquire("/tmp/cache/a", &inner) == &cache);
    table.Release(&cache);
    REQUIRE(table.BeginUnmount("/tmp/cache", &driver) == MountResult::Done);
    table.FinishUnmount("/tmp/cache");
    REQUIRE(table.Find("/tmp/cache") == nullptr);
    REQUIRE(table.Acquire("/tmp/cache/a", &inner) == &tmp);
    REQUIRE(std::string(inner) == "cache/a");
    table.Release(&tmp);

    REQUIRE(table.BeginUnmount("/tmp", &driver) == MountResult::Done);
    table.FinishUnmount("/tmp");
    REQUIRE(table.BeginUnmount("/", &driver) == MountResult::Done);
    table.FinishUnmount("/");
    REQUIRE(table.Count() == 0);
}

TEST_CASE("releasing a driver that is not mounted, or has no users, changes nothing")
{
    FakeDriver root;
    FakeDriver other;
    MountTable table;
    REQUIRE(table.Add("/", &root));
    table.Release(&other);
    table.Release(&root);
    Driver* driver = nullptr;
    REQUIRE(table.BeginUnmount("/", &driver) == MountResult::Done);
}

TEST_CASE("the type registry finds instances by type and source")
{
    FakeDriver etc;
    FakeDriver tmp;
    FakeDriver disk;
    TypeRegistry types;
    REQUIRE(types.Register("tmpfs", "etc", &etc));
    REQUIRE(types.Register("tmpfs", "tmp", &tmp));
    REQUIRE(types.Register("fat16", "vda1", &disk));
    REQUIRE(types.Register("tmpfs", "tmp", &tmp));
    REQUIRE_FALSE(types.Register("tmpfs", "tmp", &disk));
    REQUIRE_FALSE(types.Register("ext2", "vda2", &tmp));
    REQUIRE_FALSE(types.Register("tmpfs", "vda1", &disk));
    REQUIRE_FALSE(types.Register("", "x", &disk));
    REQUIRE_FALSE(types.Register("ext2", "", &disk));

    Driver* driver = nullptr;
    REQUIRE(types.Find("tmpfs", "tmp", &driver) == TypeRegistry::Lookup::Found);
    REQUIRE(driver == &tmp);
    REQUIRE(types.Find("tmpfs", "etc", &driver) == TypeRegistry::Lookup::Found);
    REQUIRE(driver == &etc);
    REQUIRE(types.Find("tmpfs", "vda1", &driver) == TypeRegistry::Lookup::UnknownSource);
    REQUIRE(types.Find("ext2", "vda1", &driver) == TypeRegistry::Lookup::UnknownType);
    REQUIRE(types.Find("tmp", "tmp", &driver) == TypeRegistry::Lookup::UnknownType);

    const char* type = nullptr;
    const char* source = nullptr;
    REQUIRE(types.Describe(&disk, &type, &source));
    REQUIRE(std::string(type) == "fat16");
    REQUIRE(std::string(source) == "vda1");
    FakeDriver unknown;
    REQUIRE_FALSE(types.Describe(&unknown, &type, &source));
}

TEST_CASE("the type registry gives a driver one name and forgets a driver")
{
    FakeDriver disk;
    FakeDriver other;
    TypeRegistry types;
    REQUIRE(types.Register("fat16", "vda1", &disk));
    REQUIRE(types.Register("fat16", "vda1", &disk));
    REQUIRE(types.Register("ext2", "vda2", &other));
    REQUIRE_FALSE(types.Register("fat16", "vda2", &other));
    REQUIRE_FALSE(types.Register("fat16", "vda3", &disk));
    REQUIRE_FALSE(types.Register("ext2", "vda1", &disk));
    Driver* driver = nullptr;
    REQUIRE(types.Find("fat16", "vda1", &driver) == TypeRegistry::Lookup::Found);
    REQUIRE(driver == &disk);
    REQUIRE(types.Find("fat16", "vda3", &driver) == TypeRegistry::Lookup::UnknownSource);

    types.Unregister(&disk);
    REQUIRE(types.Register("fat16", "vda3", &disk));
    REQUIRE(types.Find("fat16", "vda1", &driver) == TypeRegistry::Lookup::UnknownSource);
    REQUIRE(types.Find("fat16", "vda3", &driver) == TypeRegistry::Lookup::Found);
    REQUIRE(driver == &disk);

    types.Unregister(&disk);
    REQUIRE(types.Find("fat16", "vda3", &driver) == TypeRegistry::Lookup::UnknownType);
    REQUIRE(types.Find("ext2", "vda2", &driver) == TypeRegistry::Lookup::Found);
    REQUIRE(driver == &other);
    types.Unregister(&disk);
    REQUIRE(types.Register("fat16", "vda1", &disk));
}

TEST_CASE("the type registry holds five built-ins and sixteen volumes and no more than its capacity")
{
    constexpr uint32_t Needed = 5 + 16;
    std::array<FakeDriver, 40> drivers;
    std::array<std::string, 40> sources;
    for (uint32_t index = 0; index < sources.size(); ++index)
    {
        sources[index] = "vda" + std::to_string(index);
    }
    TypeRegistry types;
    for (uint32_t index = 0; index < Needed; ++index)
    {
        REQUIRE(types.Register("ext4", sources[index].c_str(), &drivers[index]));
    }
    Driver* found = nullptr;
    REQUIRE(types.Find("ext4", "vda20", &found) == TypeRegistry::Lookup::Found);
    REQUIRE(found == &drivers[20]);

    for (uint32_t index = Needed; index < TypeRegistry::Capacity; ++index)
    {
        REQUIRE(types.Register("ext4", sources[index].c_str(), &drivers[index]));
    }
    REQUIRE_FALSE(types.Register("ext4", "extra", &drivers[39]));
    types.Unregister(&drivers[3]);
    REQUIRE(types.Register("ext4", "extra", &drivers[39]));
}
