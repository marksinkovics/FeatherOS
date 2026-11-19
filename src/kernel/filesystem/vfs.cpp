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
constexpr char Fat16Type[] = "fat16";
constexpr char Ext2Type[] = "ext2";
constexpr char Ext3Type[] = "ext3";
constexpr char Ext4Type[] = "ext4";
constexpr uint32_t RootDirectoryMode = 0755;
constexpr uint64_t RootSerialNumber = 1;

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

// Why the last change failed. The file system lock in files.cpp orders the changes and the reads
// of it; kernel tests run alone.
constinit uint64_t last_error = User::SyscallErrorFailed;

bool Refuse(uint64_t error)
{
    last_error = error;
    return false;
}

bool Changed(Driver* driver, bool done)
{
    if (!done)
    {
        last_error = driver->LastError();
    }
    return done;
}

// A change to inner, a path on driver: a mount point itself cannot be changed.
bool MayChange(const Driver* driver, const char* inner)
{
    if (driver == nullptr)
    {
        return Refuse(User::SyscallErrorNotFound);
    }
    if (driver->IsMountedReadOnly())
    {
        return Refuse(User::SyscallErrorReadOnly);
    }
    return *inner != '\0' || Refuse(User::SyscallErrorBusy);
}

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

uint64_t DeviceOf(const Driver* driver)
{
    Sync::SpinLockGuard guard(mount_lock);
    return mounts.DeviceOf(driver);
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

// A driver mounted somewhere other than "/" keeps its volume; the others look for theirs on the
// disk again.
bool FindVolume(Filesystem::CoreDriver& driver, bool (*mount)())
{
    {
        Sync::SpinLockGuard guard(mount_lock);
        if (mounts.Holds(&driver) && mounts.Find("/") != &driver)
        {
            return driver.PartitionNumber() != 0;
        }
    }
    return mount();
}

// Registered under its partition's name, such as vda2, for mount; forgotten when the disk has no
// such volume.
void RegisterVolumeLocked(const char* type, Filesystem::CoreDriver& driver, bool found)
    REQUIRES(mount_lock)
{
    const char* source = Filesystem::Drivers::PartitionSource(driver.PartitionNumber());
    if (!found || source == nullptr || !types.Register(type, source, &driver))
    {
        types.Unregister(&driver);
    }
}

MountResult FindMountableLocked(const char* type, const char* source, const char* point,
                                Driver** driver) REQUIRES(mount_lock)
{
    switch (types.Find(type, source, driver))
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
    if (mounts.Find(point) != nullptr || mounts.Holds(*driver))
    {
        return MountResult::Busy;
    }
    return mounts.Count() == MountTable::Capacity ? MountResult::Full : MountResult::Done;
}

} // namespace

bool MountBuiltIn()
{
    Sync::SpinLockGuard guard(mount_lock);
    for (const BuiltIn& built_in : BuiltIns)
    {
        Driver* driver = &built_in.driver();
        if (!types.Register(built_in.type, built_in.source, driver))
        {
            return false;
        }
        // None of these drivers waits in PrepareMount.
        if (!mounts.Holds(driver) && (!driver->PrepareMount(Filesystem::MountMode::ReadWrite) ||
                                      !mounts.Add(built_in.point, driver)))
        {
            return false;
        }
    }
    return true;
}

size_t ChooseRoot(const size_t* partitions, size_t count, size_t requested)
{
    size_t chosen = NoRoot;
    for (size_t index = 0; index < count; ++index)
    {
        if (partitions[index] == 0)
        {
            continue;
        }
        if (requested != 0)
        {
            if (partitions[index] == requested)
            {
                return index;
            }
        }
        else if (chosen == NoRoot || partitions[index] < partitions[chosen])
        {
            chosen = index;
        }
    }
    return chosen;
}

bool MountBlockDevice(size_t requested, bool* honored)
{
    Filesystem::CoreDriver* const candidates[] = {
        &Filesystem::Drivers::Disk(),
        &Filesystem::Drivers::Ext2(),
        &Filesystem::Drivers::Ext3(),
        &Filesystem::Drivers::Ext4(),
    };
    bool (*const mount_volume[])() = {
        Filesystem::Drivers::MountDisk,
        Filesystem::Drivers::MountExt2,
        Filesystem::Drivers::MountExt3,
        Filesystem::Drivers::MountExt4,
    };
    constexpr size_t Count = sizeof(candidates) / sizeof(candidates[0]);
    size_t partitions[Count];
    const auto find_volumes = [&]
    {
        for (size_t index = 0; index < Count; ++index)
        {
            partitions[index] = FindVolume(*candidates[index], mount_volume[index])
                                    ? candidates[index]->PartitionNumber()
                                    : 0;
        }
    };
    find_volumes();
    // A volume that refuses writes is mounted read-only instead.
    size_t chosen = ChooseRoot(partitions, Count, requested);
    bool prepared =
        chosen != NoRoot && candidates[chosen]->PrepareMount(Filesystem::MountMode::ReadWrite);
    if (honored != nullptr)
    {
        *honored = requested == 0 || prepared;
    }
    if (requested != 0 && !prepared)
    {
        // A failed reopen forgets the volume.
        size_t others[Count];
        for (size_t index = 0; index < Count; ++index)
        {
            partitions[index] = candidates[index]->PartitionNumber();
            others[index] = index == chosen ? 0 : partitions[index];
        }
        chosen = ChooseRoot(others, Count, 0);
        prepared =
            chosen != NoRoot && candidates[chosen]->PrepareMount(Filesystem::MountMode::ReadWrite);
    }
    Filesystem::CoreDriver* const root = chosen == NoRoot ? nullptr : candidates[chosen];
    Sync::SpinLockGuard guard(mount_lock);
    RegisterVolumeLocked(Fat16Type, *candidates[0], partitions[0] != 0);
    RegisterVolumeLocked(Ext2Type, *candidates[1], partitions[1] != 0);
    RegisterVolumeLocked(Ext3Type, *candidates[2], partitions[2] != 0);
    RegisterVolumeLocked(Ext4Type, *candidates[3], partitions[3] != 0);
    const char* type = nullptr;
    const char* source = nullptr;
    const bool usable = prepared && types.Describe(root, &type, &source);
    Driver* mounted = mounts.Find("/");
    bool was_disk = false;
    for (Filesystem::CoreDriver* candidate : candidates)
    {
        was_disk = was_disk || mounted == candidate;
    }
    if (was_disk && (!usable || mounted != root))
    {
        mounts.Remove("/");
        mounted = nullptr;
    }
    return usable && (mounted == root || mounts.Add("/", root));
}

bool LoadEtc()
{
    char name[Initramfs::NameCapacity];
    Initramfs::File archive_file;
    for (uint32_t index = 0; Initramfs::ReadDirectory("etc", index, name, &archive_file); ++index)
    {
        uint32_t node;
        uint64_t written;
        if ((!Tmpfs::Create(Tmpfs::Root::Etc, name, &node) &&
             !(Tmpfs::Open(Tmpfs::Root::Etc, name, &node) && Tmpfs::Truncate(node, 0))) ||
            !Tmpfs::Write(node, 0, archive_file.data, archive_file.size, &written))
        {
            return false;
        }
    }
    return true;
}

bool UseRootEtc()
{
    const char* type = nullptr;
    const char* source = nullptr;
    bool read_only = true;
    if (!DescribeRoot(&type, &source, &read_only) || read_only)
    {
        return false;
    }
    Driver* const ram_copy = &Filesystem::Drivers::Etc();
    bool was_mounted = false;
    {
        Sync::SpinLockGuard guard(mount_lock);
        was_mounted = mounts.Find("/etc") == ram_copy && mounts.Remove("/etc");
    }
    bool done = IsDirectory("/etc") || MakeDirectory("/etc");
    char name[Initramfs::NameCapacity];
    Initramfs::File archive_file;
    for (uint32_t index = 0; done && Initramfs::ReadDirectory("etc", index, name, &archive_file);
         ++index)
    {
        char path[sizeof("/etc/") + Initramfs::NameCapacity];
        CopyText(path, sizeof(path), "/etc/");
        CopyText(path + sizeof("/etc/") - 1, Initramfs::NameCapacity, name);
        FileInfo info;
        if (Stat(path, &info))
        {
            continue;
        }
        FileHandle file{};
        uint64_t written = 0;
        done =
            Create(path, &file) && (Write(&file, archive_file.data, archive_file.size, &written) &&
                                    written == archive_file.size);
        Close(&file);
    }
    if (!done && was_mounted)
    {
        Sync::SpinLockGuard guard(mount_lock);
        mounts.Add("/etc", ram_copy);
    }
    return done;
}

MountResult Mount(const char* type, const char* source, const char* point,
                  Filesystem::MountMode mode)
{
    if (type == nullptr || source == nullptr || point == nullptr)
    {
        return MountResult::Invalid;
    }
    Driver* driver = nullptr;
    {
        Sync::SpinLockGuard guard(mount_lock);
        const MountResult found = FindMountableLocked(type, source, point, &driver);
        if (found != MountResult::Done)
        {
            return found;
        }
    }
    // Reopening a disk filesystem waits for the disk, so it runs outside the lock; the table is
    // checked again after.
    if (!driver->PrepareMount(mode))
    {
        return MountResult::Failed;
    }
    Sync::SpinLockGuard guard(mount_lock);
    const MountResult found = FindMountableLocked(type, source, point, &driver);
    if (found != MountResult::Done)
    {
        return found;
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
    const bool detached = driver->Detach();
    Sync::SpinLockGuard guard(mount_lock);
    if (!detached)
    {
        mounts.CancelUnmount(point);
        return MountResult::Failed;
    }
    mounts.FinishUnmount(point);
    return MountResult::Done;
}

bool DescribeRoot(const char** type, const char** source, bool* read_only)
{
    if (type == nullptr || source == nullptr || read_only == nullptr)
    {
        return false;
    }
    Sync::SpinLockGuard guard(mount_lock);
    const Driver* driver = mounts.Find("/");
    if (driver == nullptr || !types.Describe(driver, type, source))
    {
        return false;
    }
    *read_only = driver->IsMountedReadOnly();
    return true;
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
    status->read_only = driver->IsMountedReadOnly();
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
    *info = {};
    const Use use(path);
    if (use.driver == nullptr || !use.driver->Stat(use.inner, info))
    {
        // "/" with nothing mounted there holds the other mount points, and a mount point is a
        // directory even when its driver cannot describe its root.
        if (!(use.driver == nullptr ? IsRoot(path) : path[0] == '/' && *use.inner == '\0'))
        {
            return false;
        }
        *info = {};
        info->node = RootSerialNumber;
        info->mode = User::ModeDirectory | RootDirectoryMode;
        info->link_count = 1;
    }
    if (use.driver != nullptr)
    {
        info->device = DeviceOf(use.driver);
    }
    return true;
}

bool StatOpen(const FileHandle& file, FileInfo* info)
{
    if (info == nullptr || !file.open || file.driver == nullptr)
    {
        return false;
    }
    *info = {};
    if (!file.driver->StatNode(file.node, info))
    {
        return false;
    }
    if (HasSize(file))
    {
        info->size = file.size;
    }
    info->device = DeviceOf(file.driver);
    return true;
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
    if (!MayChange(use.driver, use.inner) ||
        !Changed(use.driver, use.driver->Create(use.inner, &node, &size)))
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
        return Refuse(User::SyscallErrorBadDescriptor);
    }
    if (file->driver->IsMountedReadOnly())
    {
        return Refuse(User::SyscallErrorReadOnly);
    }
    const bool written =
        Changed(file->driver, file->driver->Write(&file->node, &file->size, file->offset, buffer,
                                                  length, bytes_written));
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
        return Refuse(User::SyscallErrorBadDescriptor);
    }
    if (file->driver->IsMountedReadOnly())
    {
        return Refuse(User::SyscallErrorReadOnly);
    }
    const bool truncated =
        Changed(file->driver, file->driver->Truncate(&file->node, &file->size, size));
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
    Driver* child = nullptr;
    bool mount_point = false;
    {
        Sync::SpinLockGuard guard(mount_lock);
        mount_point = mounts.AcquireChild(path, index, entry, &child);
        if (!mount_point)
        {
            index -= mounts.ChildCount(path);
        }
    }
    if (!mount_point)
    {
        return use.driver != nullptr && use.driver->ReadDirectory(use.inner, index, entry);
    }
    // A mount point's node is its filesystem's root, as stat reports it.
    FileInfo root = {};
    if (child != nullptr && child->Stat("", &root))
    {
        entry->node = root.node;
    }
    Release(child);
    return true;
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
    return use.driver != nullptr &&
           (use.driver->IsMountedReadOnly() || use.driver->IsReadOnly(use.inner));
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
    return file.driver != nullptr && !file.driver->IsMountedReadOnly();
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
    return MayChange(use.driver, use.inner) &&
           Changed(use.driver, use.driver->MakeDirectory(use.inner));
}

bool RemoveDirectory(const char* path)
{
    if (path == nullptr)
    {
        return false;
    }
    const Use use(path);
    return MayChange(use.driver, use.inner) &&
           Changed(use.driver, use.driver->RemoveDirectory(use.inner));
}

bool Remove(const char* path)
{
    if (path == nullptr)
    {
        return false;
    }
    const Use use(path);
    return MayChange(use.driver, use.inner) && Changed(use.driver, use.driver->Remove(use.inner));
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
    if (!MayChange(use.driver, use.inner))
    {
        return false;
    }
    if (Locate(to, &to_inner) != use.driver)
    {
        return Refuse(User::SyscallErrorCrossDevice);
    }
    return MayChange(use.driver, to_inner) &&
           Changed(use.driver, use.driver->Rename(use.inner, to_inner));
}

uint32_t FirstLink(char* path, uint32_t from)
{
    if (path == nullptr || path[0] != '/')
    {
        return 0;
    }
    uint32_t end = 0;
    while (path[end] != '\0')
    {
        ++end;
    }
    // Walks back one mount at a time, from the deepest; the earliest link found wins.
    uint32_t found = 0;
    while (end > from)
    {
        const char saved = path[end];
        path[end] = '\0';
        uint32_t point_length = 0;
        {
            const Use use(path);
            if (use.driver != nullptr)
            {
                const auto inner = static_cast<uint32_t>(use.inner - path);
                point_length = *use.inner == '\0' ? end : (inner > 1 ? inner - 1 : 1);
                uint32_t link_end = 0;
                if (*use.inner != '\0' && use.driver->FindLink(use.inner, &link_end))
                {
                    found = inner + link_end;
                }
            }
        }
        path[end] = saved;
        if (point_length <= 1)
        {
            break;
        }
        // The components before the mount point belong to the mounts that hold its parent.
        uint32_t parent = point_length;
        while (parent > 0 && path[parent - 1] != '/')
        {
            --parent;
        }
        end = parent > 1 ? parent - 1 : 1;
    }
    return found;
}

bool ReadLink(const char* path, char* buffer, uint32_t capacity, uint32_t* length)
{
    if (path == nullptr || buffer == nullptr || length == nullptr)
    {
        return false;
    }
    const Use use(path);
    return use.driver != nullptr && *use.inner != '\0' &&
           use.driver->ReadLink(use.inner, buffer, capacity, length);
}

bool SymbolicLink(const char* path, const char* target)
{
    if (path == nullptr || target == nullptr)
    {
        return false;
    }
    const Use use(path);
    return MayChange(use.driver, use.inner) &&
           Changed(use.driver, use.driver->SymbolicLink(use.inner, target));
}

bool Link(const char* from, const char* to)
{
    if (from == nullptr || to == nullptr)
    {
        return false;
    }
    const Use use(from);
    const char* to_inner;
    if (!MayChange(use.driver, use.inner))
    {
        return false;
    }
    if (Locate(to, &to_inner) != use.driver)
    {
        return Refuse(User::SyscallErrorCrossDevice);
    }
    return MayChange(use.driver, to_inner) &&
           Changed(use.driver, use.driver->Link(use.inner, to_inner));
}

uint64_t LastError()
{
    return last_error;
}

namespace
{

template <typename Call> bool ForEachMount(Call call)
{
    bool done = true;
    for (uint32_t index = 0; index < MountTable::Capacity; ++index)
    {
        Driver* driver = nullptr;
        {
            Sync::SpinLockGuard guard(mount_lock);
            const char* point = nullptr;
            if (!mounts.At(index, &point, &driver))
            {
                break;
            }
            driver = mounts.AcquirePoint(point);
        }
        if (driver != nullptr)
        {
            done = call(*driver) && done;
            Release(driver);
        }
    }
    return done;
}

} // namespace

bool SyncAll()
{
    return ForEachMount([](Driver& driver) { return driver.Sync(); });
}

bool SyncPeriodic()
{
    return ForEachMount([](Driver& driver)
                        { return !driver.SyncsInBackground() || driver.Sync(); });
}

bool DetachAll()
{
    return ForEachMount([](Driver& driver) { return driver.Detach(); });
}

bool SupportsLinks(const char* path, Filesystem::LinkKind kind)
{
    if (path == nullptr)
    {
        return false;
    }
    const Use use(path);
    return use.driver != nullptr && use.driver->SupportsLinks(kind);
}

} // namespace FeatherOS::VFS
