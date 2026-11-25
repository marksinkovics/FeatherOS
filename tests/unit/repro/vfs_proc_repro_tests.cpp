#include <catch2/catch_test_macros.hpp>

#include "kernel/files.h"
#include "kernel/filesystem/procfs.h"
#include "kernel/filesystem/vfs.h"

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

bool LongMountAt(uint32_t index, Procfs::MountStatus* status)
{
    if (index != 0)
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

} // namespace

TEST_CASE("R2-08 a mount point on an existing directory is listed once", "[.repro][R2-08]")
{
    REQUIRE(Files::MakeDirectory("/etc/usb") == 0);
    {
        const TmpMovedInto moved("/etc/usb");
        REQUIRE(Files::Mount("tmp", "/etc/usb", "tmpfs") == 0);
        int listed = 0;
        for (const std::string& name : Names("/etc"))
        {
            listed += name == "usb" ? 1 : 0;
        }
        CHECK(listed == 1);
    }
    Files::RemoveDirectory("/etc/usb");
}

TEST_CASE("R2-09 renaming a directory with a mount point inside is refused or moves the mount",
          "[.repro][R2-09]")
{
    REQUIRE(Files::MakeDirectory("/etc/outer") == 0);
    REQUIRE(Files::MakeDirectory("/etc/outer/vol") == 0);
    bool renamed = false;
    {
        const TmpMovedInto moved("/etc/outer/vol");
        REQUIRE(Files::Mount("tmp", "/etc/outer/vol", "tmpfs") == 0);
        renamed = Files::Rename("/etc/outer", "/etc/moved") == 0;
        if (renamed)
        {
            // The mount must have followed its directory; the table matches path text.
            CHECK(VFS::MountedAt("/etc/outer/vol") == nullptr);
            CHECK(VFS::MountedAt("/etc/moved/vol") != nullptr);
        }
        if (VFS::MountedAt("/etc/moved/vol") != nullptr)
        {
            Files::Unmount("/etc/moved/vol", NoWorkingDirectory);
        }
        else if (VFS::MountedAt("/etc/outer/vol") != nullptr)
        {
            Files::Unmount("/etc/outer/vol", NoWorkingDirectory);
        }
    }
    if (renamed)
    {
        Files::RemoveDirectory("/etc/moved/vol");
        Files::RemoveDirectory("/etc/moved");
    }
    else
    {
        Files::RemoveDirectory("/etc/outer/vol");
        Files::RemoveDirectory("/etc/outer");
    }
}

TEST_CASE("R2-10 /proc/mounts opens when a mount point is long", "[.repro][R2-10]")
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
