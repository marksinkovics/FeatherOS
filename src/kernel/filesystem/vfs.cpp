#include "vfs.h"

#include "drivers.h"
#include "fat16.h"
#include "initramfs.h"
#include "mount_table.h"
#include "tmpfs.h"

#include "kernel/sync/spinlock.h"

namespace FeatherOS::VFS
{
namespace
{

using Filesystem::Driver;

constexpr char SystemPoint[] = "/system";

struct BuiltIn
{
    const char* point;
    Driver& (*driver)();
};

constexpr BuiltIn BuiltIns[] = {
    {"/system", Filesystem::Drivers::System}, {"/etc", Filesystem::Drivers::Etc},
    {"/dev", Filesystem::Drivers::Devices},   {"/proc", Filesystem::Drivers::Proc},
    {"/tmp", Filesystem::Drivers::Tmp},
};

// Held only while the table is read or changed, never across a driver call, since drivers wait
// for the disk.
constinit Sync::SpinLock mount_lock;
constinit MountTable mounts GUARDED_BY(mount_lock);

// The bare names kernel code uses ("init") name files in /system.
Driver* LocateLocked(const char* path, const char** inner) REQUIRES(mount_lock)
{
    if (path[0] == '/')
    {
        return mounts.Resolve(path, inner);
    }
    if (path[0] == '\0')
    {
        return nullptr;
    }
    *inner = path;
    return mounts.Find(SystemPoint);
}

Driver* Locate(const char* path, const char** inner)
{
    Sync::SpinLockGuard guard(mount_lock);
    return LocateLocked(path, inner);
}

// "/" exists even with nothing mounted there, since it holds the other mount points.
bool IsRoot(const char* path)
{
    if (path[0] != '/')
    {
        return false;
    }
    while (*path == '/')
    {
        ++path;
    }
    return *path == '\0';
}

bool HasSize(const FileHandle& file)
{
    return !file.driver->Attributes().devices;
}

} // namespace

bool MountBuiltIn()
{
    Sync::SpinLockGuard guard(mount_lock);
    for (const BuiltIn& built_in : BuiltIns)
    {
        Driver* driver = &built_in.driver();
        if (mounts.Find(built_in.point) != driver && !mounts.Add(built_in.point, driver))
        {
            return false;
        }
    }
    return true;
}

bool MountBlockDevice()
{
    Unmount("/");
    return FAT16::Mount() && Mount("/", &Filesystem::Drivers::Disk());
}

bool LoadEtc()
{
    char name[64];
    uint64_t size;
    for (uint32_t index = 0; Initramfs::ReadDirectory("etc", index, name, &size); ++index)
    {
        Initramfs::File archive_file;
        uint32_t node;
        uint64_t written;
        if (!Initramfs::Open("etc", name, &archive_file) ||
            (!Tmpfs::Create(Tmpfs::Root::Etc, name, &node) &&
             !(Tmpfs::Open(Tmpfs::Root::Etc, name, &node) && Tmpfs::Truncate(node, 0))) ||
            !Tmpfs::Write(node, 0, archive_file.data, archive_file.size, &written))
        {
            return false;
        }
    }
    return true;
}

bool Mount(const char* point, Driver* driver)
{
    Sync::SpinLockGuard guard(mount_lock);
    return mounts.Add(point, driver);
}

bool Unmount(const char* point)
{
    Sync::SpinLockGuard guard(mount_lock);
    return mounts.Remove(point);
}

bool Open(const char* path, FileHandle* file)
{
    if (path == nullptr || file == nullptr)
    {
        return false;
    }
    const char* inner;
    Driver* driver = Locate(path, &inner);
    Filesystem::Node node = {};
    uint64_t size = 0;
    if (driver == nullptr || *inner == '\0' || !driver->Open(inner, &node, &size))
    {
        return false;
    }
    *file = {driver, node, size, 0, true};
    return true;
}

bool Read(FileHandle* file, void* buffer, uint64_t length, uint64_t* bytes_read)
{
    if (bytes_read == nullptr)
    {
        return false;
    }
    *bytes_read = 0;
    if (file == nullptr || !file->open || file->driver == nullptr ||
        (length > 0 && buffer == nullptr) || file->offset > file->size ||
        !file->driver->Read(file->node, file->size, file->offset, buffer, length, bytes_read))
    {
        return false;
    }
    if (HasSize(*file))
    {
        file->offset += *bytes_read;
    }
    return true;
}

bool Seek(FileHandle* file, uint64_t offset)
{
    if (file == nullptr || !file->open || file->driver == nullptr ||
        (HasSize(*file) && offset > file->size))
    {
        return false;
    }
    file->offset = HasSize(*file) ? offset : 0;
    return true;
}

bool Stat(const char* path, FileInfo* info)
{
    if (path == nullptr || info == nullptr)
    {
        return false;
    }
    info->device = false;
    const char* inner;
    Driver* driver = Locate(path, &inner);
    return driver != nullptr && *inner != '\0' && driver->Stat(inner, info);
}

void Close(FileHandle* file)
{
    if (file == nullptr)
    {
        return;
    }
    if (file->open && file->driver != nullptr)
    {
        file->driver->Close(file->node);
    }
    *file = {};
}

bool Create(const char* path, FileHandle* file)
{
    if (path == nullptr || file == nullptr)
    {
        return false;
    }
    const char* inner;
    Driver* driver = Locate(path, &inner);
    Filesystem::Node node = {};
    uint64_t size = 0;
    if (driver == nullptr || *inner == '\0' || !driver->Create(inner, &node, &size))
    {
        return false;
    }
    *file = {driver, node, size, 0, true};
    return true;
}

bool Write(FileHandle* file, const void* buffer, uint64_t length, uint64_t* bytes_written)
{
    if (bytes_written == nullptr)
    {
        return false;
    }
    *bytes_written = 0;
    if (file == nullptr || !file->open || file->driver == nullptr)
    {
        return false;
    }
    const bool written =
        file->driver->Write(&file->node, &file->size, file->offset, buffer, length, bytes_written);
    if (HasSize(*file))
    {
        file->offset += *bytes_written;
    }
    return written;
}

bool Truncate(FileHandle* file, uint64_t size)
{
    if (file == nullptr || !file->open || file->driver == nullptr)
    {
        return false;
    }
    const bool truncated = file->driver->Truncate(&file->node, &file->size, size);
    if (file->offset > file->size)
    {
        file->offset = file->size;
    }
    return truncated;
}

bool ReadDirectory(const char* path, uint32_t index, Filesystem::DirectoryEntry* entry)
{
    if (path == nullptr || entry == nullptr)
    {
        return false;
    }
    const char* inner;
    Driver* driver;
    {
        Sync::SpinLockGuard guard(mount_lock);
        driver = LocateLocked(path, &inner);
        if (mounts.ChildAt(path, index, entry))
        {
            return true;
        }
        index -= mounts.ChildCount(path);
    }
    return driver != nullptr && driver->ReadDirectory(inner, index, entry);
}

bool IsDirectory(const char* path)
{
    if (path == nullptr)
    {
        return false;
    }
    const char* inner;
    Driver* driver = Locate(path, &inner);
    if (driver == nullptr)
    {
        return IsRoot(path);
    }
    return (path[0] == '/' && *inner == '\0') || driver->IsDirectory(inner);
}

bool IsReadOnly(const char* path)
{
    if (path == nullptr)
    {
        return false;
    }
    const char* inner;
    Driver* driver = Locate(path, &inner);
    return driver != nullptr && driver->IsReadOnly(inner);
}

bool IsMountPoint(const char* path)
{
    if (path == nullptr || path[0] != '/')
    {
        return false;
    }
    const char* inner;
    return Locate(path, &inner) == nullptr ? IsRoot(path) : *inner == '\0';
}

bool IsTracked(const FileHandle& file)
{
    return file.open && file.driver != nullptr && file.driver->Attributes().tracked;
}

FileKey KeyOf(const FileHandle& file)
{
    return {file.driver, file.node.id};
}

bool IsWritable(const FileHandle& file)
{
    return file.driver != nullptr && !file.driver->Attributes().read_only;
}

bool IsDevice(const FileHandle& file)
{
    return file.driver != nullptr && file.driver->Attributes().devices;
}

bool DeviceNode(const FileHandle& file, uint32_t* node)
{
    if (node == nullptr || !IsDevice(file))
    {
        return false;
    }
    *node = static_cast<uint32_t>(file.node.id);
    return true;
}

bool MakeDirectory(const char* path)
{
    if (path == nullptr)
    {
        return false;
    }
    const char* inner;
    Driver* driver = Locate(path, &inner);
    return driver != nullptr && *inner != '\0' && driver->MakeDirectory(inner);
}

bool RemoveDirectory(const char* path)
{
    if (path == nullptr)
    {
        return false;
    }
    const char* inner;
    Driver* driver = Locate(path, &inner);
    return driver != nullptr && *inner != '\0' && driver->RemoveDirectory(inner);
}

bool Remove(const char* path)
{
    if (path == nullptr)
    {
        return false;
    }
    const char* inner;
    Driver* driver = Locate(path, &inner);
    return driver != nullptr && *inner != '\0' && driver->Remove(inner);
}

bool SameFilesystem(const char* left, const char* right)
{
    if (left == nullptr || right == nullptr)
    {
        return false;
    }
    const char* inner;
    return Locate(left, &inner) == Locate(right, &inner);
}

bool Rename(const char* from, const char* to)
{
    if (from == nullptr || to == nullptr)
    {
        return false;
    }
    const char* from_inner;
    const char* to_inner;
    Driver* driver = Locate(from, &from_inner);
    return driver != nullptr && Locate(to, &to_inner) == driver && *from_inner != '\0' &&
           *to_inner != '\0' && driver->Rename(from_inner, to_inner);
}

} // namespace FeatherOS::VFS
