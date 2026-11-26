#include <catch2/catch_test_macros.hpp>

#include "kernel/files.h"
#include "kernel/filesystem/procfs.h"
#include "kernel/filesystem/vfs.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace
{

namespace Files = FeatherOS::Files;
namespace VFS = FeatherOS::VFS;
namespace Procfs = FeatherOS::Procfs;

bool NoWorkingDirectory(const char* /*point*/)
{
    return false;
}

std::vector<std::string> Names(const char* directory)
{
    std::vector<std::string> names;
    FeatherOS::Filesystem::DirectoryEntry entry{};
    for (uint32_t index = 0; VFS::ReadDirectory(directory, index, &entry); ++index)
    {
        names.emplace_back(entry.name);
    }
    return names;
}

// "tmp" leaves /tmp for a point inside /etc for the test, and returns to /tmp afterwards.
class TmpMovedInto
{
public:
    explicit TmpMovedInto(const char* point) : point_(point)
    {
        REQUIRE(Files::Unmount("/tmp", NoWorkingDirectory) == 0);
        moved_ = true;
    }

    TmpMovedInto(const TmpMovedInto&) = delete;
    TmpMovedInto& operator=(const TmpMovedInto&) = delete;

    ~TmpMovedInto()
    {
        if (moved_)
        {
            Files::Unmount(point_, NoWorkingDirectory);
            Files::Mount("tmp", "/tmp", "tmpfs");
        }
    }

private:
    const char* point_;
    bool moved_ = false;
};

class SourceRestored
{
public:
    SourceRestored() = default;
    SourceRestored(const SourceRestored&) = delete;
    SourceRestored& operator=(const SourceRestored&) = delete;

    ~SourceRestored()
    {
        Procfs::SetSource({});
    }
};

bool LongMount(uint32_t index, uint32_t count, Procfs::MountStatus* status)
{
    if (index >= count)
    {
        return false;
    }
    *status = {};
    status->source = "vda1";
    status->type = "ext4";
    status->point[0] = '/';
    for (uint32_t at = 1; at < 1023; ++at)
    {
        status->point[at] = 'a';
    }
    status->read_only = false;
    return true;
}

bool LongMountAt(uint32_t index, Procfs::MountStatus* status)
{
    return LongMount(index, 1, status);
}

bool SixteenLongMountsAt(uint32_t index, Procfs::MountStatus* status)
{
    return LongMount(index, 16, status);
}

} // namespace

TEST_CASE("R2-08 a mount point on an existing directory is listed once", "[R2-08]")
{
    REQUIRE(Files::MakeDirectory("/etc/usb") == 0);
    REQUIRE(Files::MakeDirectory("/etc/other") == 0);
    {
        const TmpMovedInto moved("/etc/usb");
        REQUIRE(Files::Mount("tmp", "/etc/usb", "tmpfs") == 0);
        int listed = 0;
        for (const std::string& name : Names("/etc"))
        {
            listed += name == "usb" ? 1 : 0;
        }
        CHECK(listed == 1);
        const std::vector<std::string> names = Names("/etc");
        CHECK(std::set<std::string>(names.begin(), names.end()).size() == names.size());
        CHECK(std::count(names.begin(), names.end(), "other") == 1);
    }
    Files::RemoveDirectory("/etc/other");
    Files::RemoveDirectory("/etc/usb");
}

TEST_CASE("R2-09 renaming a directory with a mount point inside is refused", "[R2-09]")
{
    namespace User = FeatherOS::User;
    REQUIRE(Files::MakeDirectory("/etc/outer") == 0);
    REQUIRE(Files::MakeDirectory("/etc/outer/vol") == 0);
    {
        const TmpMovedInto moved("/etc/outer/vol");
        REQUIRE(Files::Mount("tmp", "/etc/outer/vol", "tmpfs") == 0);
        CHECK(Files::Rename("/etc/outer", "/etc/moved") == User::SyscallErrorBusy);
        CHECK(VFS::MountedAt("/etc/outer/vol") != nullptr);
        CHECK(VFS::MountedAt("/etc/moved/vol") == nullptr);
        CHECK_FALSE(VFS::Rename("/etc/outer", "/etc/moved"));
        CHECK(VFS::MountedAt("/etc/outer/vol") != nullptr);
        Files::Unmount("/etc/outer/vol", NoWorkingDirectory);
    }
    CHECK(Files::Rename("/etc/outer", "/etc/moved") == 0);
    CHECK(Files::RemoveDirectory("/etc/moved/vol") == 0);
    CHECK(Files::RemoveDirectory("/etc/moved") == 0);
}

TEST_CASE("R2-10 /proc/mounts opens when a mount point is long", "[R2-10]")
{
    const SourceRestored restore;
    Procfs::SetSource({nullptr, nullptr, nullptr, nullptr, LongMountAt});

    std::string buffer(Procfs::FileCapacity("mounts"), '\0');
    size_t length = 0;
    CHECK(Procfs::Generate("mounts", buffer.data(), &length));

    VFS::FileHandle file{};
    const bool opened = VFS::Open("/proc/mounts", &file);
    CHECK(opened);
    if (opened)
    {
        VFS::Close(&file);
    }
    FeatherOS::Filesystem::FileInfo info{};
    CHECK(VFS::Stat("/proc/mounts", &info));
}

TEST_CASE("R2-10 /proc/mounts reads in full with sixteen long mount points", "[R2-10]")
{
    const SourceRestored restore;
    Procfs::SetSource({nullptr, nullptr, nullptr, nullptr, SixteenLongMountsAt});

    VFS::FileHandle file{};
    REQUIRE(VFS::Open("/proc/mounts", &file));
    std::string text;
    char chunk[512];
    uint64_t count = 0;
    while (VFS::Read(&file, chunk, sizeof(chunk), &count) && count > 0)
    {
        text.append(chunk, count);
    }
    VFS::Close(&file);
    CHECK(text.size() > 16 * 1000);
    CHECK(text.back() == '\n');
}
