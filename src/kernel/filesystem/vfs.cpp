#include "vfs.h"

#include "drivers.h"
#include "initramfs.h"
#include "mount_table.h"
#include "procfs.h"
#include "tmpfs.h"
#include "type_registry.h"

#include "kernel/sync/spinlock.h"

namespace FeatherOS::VFS
{
namespace
{

using Filesystem::Driver;

constexpr char SystemPoint[] = "/system";
constexpr char DiskType[] = "fat16";
constexpr char Ext2Type[] = "ext2";

struct BuiltIn
{
    const char* type;
    const char* source;
    const char* point;
    Driver& (*driver)();
};

constexpr BuiltIn BuiltIns[] = {
    {"initramfs", "initramfs", "/system", Filesystem::Drivers::System},
    {"tmpfs", "etc", "/etc", Filesystem::Drivers::Etc},
    {"devfs", "devfs", "/dev", Filesystem::Drivers::Devices},
    {"procfs", "procfs", "/proc", Filesystem::Drivers::Proc},
    {"tmpfs", "tmp", "/tmp", Filesystem::Drivers::Tmp},
};

// Held only while the tables are read or changed, never across a driver call, since drivers wait
// for the disk. Mount users keep a driver from being unmounted meanwhile (see MountTable).
constinit Sync::SpinLock mount_lock;
constinit MountTable mounts GUARDED_BY(mount_lock);
constinit TypeRegistry types GUARDED_BY(mount_lock);

// The bare names kernel code uses ("init") name files in /system.
Driver* AcquireLocked(const char* path, const char** inner) REQUIRES(mount_lock)
{
    if (path[0] == '/')
    {
        return mounts.Acquire(path, inner);
    }
    if (path[0] == '\0')
    {
        return nullptr;
    }
    *inner = path;
    return mounts.AcquirePoint(SystemPoint);
}

void Release(const Driver* driver)
{
    if (driver != nullptr)
    {
        Sync::SpinLockGuard guard(mount_lock);
        mounts.Release(driver);
    }
}

// The driver for a path, kept from being unmounted while this lives.
class Use
{
public:
    explicit Use(const char* path)
    {
        Sync::SpinLockGuard guard(mount_lock);
        driver = AcquireLocked(path, &inner);
    }
    ~Use()
    {
        Release(driver);
    }
    Use(const Use&) = delete;
    Use& operator=(const Use&) = delete;
    Use(Use&&) = delete;
    Use& operator=(Use&&) = delete;

    // For a handle, which holds the mount until Close.
    void Keep()
    {
        driver = nullptr;
    }

    Driver* driver = nullptr;
    const char* inner = nullptr;
};

// For comparing mounts only; the driver must not be called.
Driver* Locate(const char* path, const char** inner)
{
    Sync::SpinLockGuard guard(mount_lock);
    if (path[0] == '/')
    {
        return mounts.Resolve(path, inner);
    }
    *inner = path;
    return path[0] == '\0' ? nullptr : mounts.Find(SystemPoint);
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

void CopyText(char* destination, size_t capacity, const char* source)
{
    size_t index = 0;
    for (; source[index] != '\0' && index + 1 < capacity; ++index)
    {
        destination[index] = source[index];
    }
    destination[index] = '\0';
}

// Read-only and left out of the tree at boot; mount names it by its partition, such as vda2.
void RegisterExt2()
{
    Filesystem::CoreDriver& ext2 = Filesystem::Drivers::Ext2();
    {
        Sync::SpinLockGuard guard(mount_lock);
        if (mounts.Holds(&ext2))
        {
            return;
        }
    }
    const bool mounted = Filesystem::Drivers::MountExt2();
    const char* source = Filesystem::Drivers::PartitionSource(ext2.PartitionNumber());
    Sync::SpinLockGuard guard(mount_lock);
    if (!mounted || source == nullptr || !types.Register(Ext2Type, source, &ext2))
    {
        types.Unregister(&ext2);
    }
}

} // namespace

bool MountBuiltIn()
{
    Sync::SpinLockGuard guard(mount_lock);
    for (const BuiltIn& built_in : BuiltIns)
    {
        Driver* driver = &built_in.driver();
        if (!types.Register(built_in.type, built_in.source, driver) ||
            (!mounts.Holds(driver) && !mounts.Add(built_in.point, driver)))
        {
            return false;
        }
    }
    return true;
}

bool MountBlockDevice()
{
    const bool mounted = Filesystem::Drivers::MountDisk();
    RegisterExt2();
    Filesystem::CoreDriver* driver = &Filesystem::Drivers::Disk();
    const char* source = Filesystem::Drivers::PartitionSource(driver->PartitionNumber());
    Sync::SpinLockGuard guard(mount_lock);
    if (!mounted || source == nullptr)
    {
        if (mounts.Find("/") == driver)
        {
            mounts.Remove("/");
        }
        return false;
    }
    if (!types.Register(DiskType, source, driver))
    {
        return false;
    }
    return mounts.Find("/") == driver || mounts.Add("/", driver);
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

MountResult Mount(const char* type, const char* source, const char* point)
{
    if (type == nullptr || source == nullptr || point == nullptr)
    {
        return MountResult::Invalid;
    }
    Sync::SpinLockGuard guard(mount_lock);
    Driver* driver = nullptr;
    switch (types.Find(type, source, &driver))
    {
    case TypeRegistry::Lookup::UnknownType:
        return MountResult::UnknownType;
    case TypeRegistry::Lookup::UnknownSource:
        return MountResult::NotFound;
    case TypeRegistry::Lookup::Found:
        break;
    }
    if (!MountTable::IsValidPoint(point))
    {
        return MountResult::Invalid;
    }
    if (mounts.Find(point) != nullptr || mounts.Holds(driver))
    {
        return MountResult::Busy;
    }
    if (mounts.Count() == MountTable::Capacity)
    {
        return MountResult::Full;
    }
    return mounts.Add(point, driver) ? MountResult::Done : MountResult::Invalid;
}

MountResult Unmount(const char* point)
{
    Driver* driver = nullptr;
    {
        Sync::SpinLockGuard guard(mount_lock);
        const MountResult begun = mounts.BeginUnmount(point, &driver);
        if (begun != MountResult::Done)
        {
            return begun;
        }
    }
    const bool synced = driver->Sync();
    Sync::SpinLockGuard guard(mount_lock);
    if (!synced)
    {
        mounts.CancelUnmount(point);
        return MountResult::Failed;
    }
    mounts.FinishUnmount(point);
    return MountResult::Done;
}

bool DescribeMount(uint32_t index, Procfs::MountStatus* status)
{
    if (status == nullptr)
    {
        return false;
    }
    Sync::SpinLockGuard guard(mount_lock);
    const char* point = nullptr;
    Driver* driver = nullptr;
    if (!mounts.At(index, &point, &driver))
    {
        return false;
    }
    if (!types.Describe(driver, &status->type, &status->source))
    {
        status->type = "unknown";
        status->source = "none";
    }
    CopyText(status->point, sizeof(status->point), point);
    status->read_only = driver->Attributes().read_only;
    return true;
}

bool Open(const char* path, FileHandle* file)
{
    if (path == nullptr || file == nullptr)
    {
        return false;
    }
    Use use(path);
    Filesystem::Node node = {};
    uint64_t size = 0;
    if (use.driver == nullptr || *use.inner == '\0' || !use.driver->Open(use.inner, &node, &size))
    {
        return false;
    }
    *file = {use.driver, node, size, 0, true};
    use.Keep();
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
    const Use use(path);
    return use.driver != nullptr && *use.inner != '\0' && use.driver->Stat(use.inner, info);
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
        Release(file->driver);
    }
    *file = {};
}

bool Create(const char* path, FileHandle* file)
{
    if (path == nullptr || file == nullptr)
    {
        return false;
    }
    Use use(path);
    Filesystem::Node node = {};
    uint64_t size = 0;
    if (use.driver == nullptr || *use.inner == '\0' || !use.driver->Create(use.inner, &node, &size))
    {
        return false;
    }
    *file = {use.driver, node, size, 0, true};
    use.Keep();
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
    const Use use(path);
    {
        Sync::SpinLockGuard guard(mount_lock);
        if (mounts.ChildAt(path, index, entry))
        {
            return true;
        }
        index -= mounts.ChildCount(path);
    }
    return use.driver != nullptr && use.driver->ReadDirectory(use.inner, index, entry);
}

bool IsDirectory(const char* path)
{
    if (path == nullptr)
    {
        return false;
    }
    const Use use(path);
    if (use.driver == nullptr)
    {
        return IsRoot(path);
    }
    return (path[0] == '/' && *use.inner == '\0') || use.driver->IsDirectory(use.inner);
}

bool IsReadOnly(const char* path)
{
    if (path == nullptr)
    {
        return false;
    }
    const Use use(path);
    return use.driver != nullptr && use.driver->IsReadOnly(use.inner);
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
    const Use use(path);
    return use.driver != nullptr && *use.inner != '\0' && use.driver->MakeDirectory(use.inner);
}

bool RemoveDirectory(const char* path)
{
    if (path == nullptr)
    {
        return false;
    }
    const Use use(path);
    return use.driver != nullptr && *use.inner != '\0' && use.driver->RemoveDirectory(use.inner);
}

bool Remove(const char* path)
{
    if (path == nullptr)
    {
        return false;
    }
    const Use use(path);
    return use.driver != nullptr && *use.inner != '\0' && use.driver->Remove(use.inner);
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
    const Use use(from);
    const char* to_inner;
    return use.driver != nullptr && Locate(to, &to_inner) == use.driver && *use.inner != '\0' &&
           *to_inner != '\0' && use.driver->Rename(use.inner, to_inner);
}

} // namespace FeatherOS::VFS
