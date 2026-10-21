#include "files.h"

#include "kernel/sync/mutex.h"

namespace FeatherOS::Files
{
namespace
{

constexpr uint64_t KnownFlags =
    User::OpenRead | User::OpenWrite | User::OpenCreate | User::OpenTruncate | User::OpenAppend;
constexpr uint32_t MaximumOpenDiskFiles = 64;

// Disk files open anywhere in the system, keyed by where their directory entry is. A VFS handle
// caches the file's size and first cluster, so a second writer, or removing an open file, would act
// on stale state.
struct DiskFileUse
{
    uint64_t key;
    uint32_t readers;
    bool writer;
};

// Filesystem updates span many sectors; one lock keeps them from interleaving.
Sync::Mutex file_system_lock;
DiskFileUse disk_files[MaximumOpenDiskFiles];
uint32_t disk_file_count = 0;

uint64_t UseKey(const VFS::FileHandle& handle)
{
    return (static_cast<uint64_t>(handle.directory_cluster) << 32) | handle.entry_index;
}

DiskFileUse* FindUse(uint64_t key)
{
    for (uint32_t index = 0; index < disk_file_count; ++index)
    {
        if (disk_files[index].key == key)
        {
            return &disk_files[index];
        }
    }
    return nullptr;
}

uint64_t AddUse(uint64_t key, bool writable)
{
    DiskFileUse* use = FindUse(key);
    if (use != nullptr)
    {
        if (writable || use->writer)
        {
            return User::SyscallErrorBusy;
        }
        ++use->readers;
        return 0;
    }
    if (disk_file_count == MaximumOpenDiskFiles)
    {
        return User::SyscallErrorTooManyOpenFiles;
    }
    disk_files[disk_file_count++] = {key, writable ? 0U : 1U, writable};
    return 0;
}

void DropUse(uint64_t key, bool writable)
{
    DiskFileUse* use = FindUse(key);
    if (use == nullptr)
    {
        return;
    }
    if (writable)
    {
        use->writer = false;
    }
    else if (use->readers > 0)
    {
        --use->readers;
    }
    if (!use->writer && use->readers == 0)
    {
        *use = disk_files[--disk_file_count];
    }
}

OpenFile* FindOpenFile(Table* table, uint64_t descriptor)
{
    if (table == nullptr || descriptor < User::FirstFileDescriptor ||
        descriptor - User::FirstFileDescriptor >= User::MaximumOpenFiles)
    {
        return nullptr;
    }
    OpenFile& file = table->files[descriptor - User::FirstFileDescriptor];
    return file.used ? &file : nullptr;
}

void CloseLocked(OpenFile* file)
{
    if (file->handle.block_backed)
    {
        DropUse(UseKey(file->handle), file->writable);
    }
    VFS::Close(&file->handle);
    file->used = false;
    file->readable = false;
    file->writable = false;
}

bool ExistsLocked(const char* path)
{
    VFS::FileHandle handle;
    if (VFS::IsDirectory(path))
    {
        return true;
    }
    if (!VFS::Open(path, &handle))
    {
        return false;
    }
    VFS::Close(&handle);
    return true;
}

// Paths reach Files already resolved to absolute form, so the parent is everything before the
// last '/'.
bool ParentExistsLocked(const char* path)
{
    char parent[User::MaximumPathLength];
    uint64_t last_separator = 0;
    uint64_t length = 0;
    for (; path[length] != '\0' && length < sizeof(parent) - 1; ++length)
    {
        parent[length] = path[length];
        if (path[length] == '/')
        {
            last_separator = length;
        }
    }
    parent[last_separator == 0 ? 1 : last_separator] = '\0';
    return VFS::IsDirectory(parent);
}

// Why an operation that changes path could not create it.
uint64_t CreateErrorLocked(const char* path)
{
    if (ExistsLocked(path))
    {
        return VFS::IsDirectory(path) ? User::SyscallErrorIsDirectory : User::SyscallErrorExists;
    }
    if (VFS::IsReadOnly(path))
    {
        return User::SyscallErrorReadOnly;
    }
    return ParentExistsLocked(path) ? User::SyscallErrorFailed : User::SyscallErrorNotFound;
}

uint64_t MissingErrorLocked(const char* path)
{
    return ExistsLocked(path) ? User::SyscallErrorNotDirectory : User::SyscallErrorNotFound;
}

} // namespace

uint64_t Open(Table* table, const char* path, uint64_t flags)
{
    const bool writable = (flags & User::OpenWrite) != 0;
    if (table == nullptr || path == nullptr || (flags & ~KnownFlags) != 0 ||
        (flags & (User::OpenRead | User::OpenWrite)) == 0 ||
        ((flags & (User::OpenCreate | User::OpenTruncate | User::OpenAppend)) != 0 && !writable))
    {
        return User::SyscallErrorInvalidArgument;
    }

    Sync::MutexGuard guard(file_system_lock);
    uint64_t slot = 0;
    while (slot < User::MaximumOpenFiles && table->files[slot].used)
    {
        ++slot;
    }
    if (slot == User::MaximumOpenFiles)
    {
        return User::SyscallErrorTooManyOpenFiles;
    }

    VFS::FileHandle handle;
    if (!VFS::Open(path, &handle))
    {
        if (VFS::IsDirectory(path))
        {
            return User::SyscallErrorIsDirectory;
        }
        if ((flags & User::OpenCreate) == 0)
        {
            return User::SyscallErrorNotFound;
        }
        if (!VFS::Create(path, &handle))
        {
            return CreateErrorLocked(path);
        }
    }
    else if (writable && !handle.block_backed)
    {
        VFS::Close(&handle);
        return User::SyscallErrorReadOnly;
    }
    if (handle.block_backed)
    {
        const uint64_t added = AddUse(UseKey(handle), writable);
        if (User::IsSyscallError(added))
        {
            VFS::Close(&handle);
            return added;
        }
    }

    OpenFile& file = table->files[slot];
    file.handle = handle;
    file.used = true;
    file.readable = (flags & User::OpenRead) != 0;
    file.writable = writable;
    if ((flags & User::OpenTruncate) != 0 && !VFS::Truncate(&file.handle, 0))
    {
        CloseLocked(&file);
        return User::SyscallErrorFailed;
    }
    if ((flags & User::OpenAppend) != 0)
    {
        VFS::Seek(&file.handle, file.handle.size);
    }
    return User::FirstFileDescriptor + slot;
}

uint64_t Read(Table* table, uint64_t descriptor, void* buffer, uint64_t length)
{
    Sync::MutexGuard guard(file_system_lock);
    OpenFile* file = FindOpenFile(table, descriptor);
    if (file == nullptr || !file->readable)
    {
        return User::SyscallErrorBadDescriptor;
    }
    uint64_t bytes_read;
    return VFS::Read(&file->handle, buffer, length, &bytes_read) ? bytes_read
                                                                 : User::SyscallErrorFailed;
}

uint64_t Write(Table* table, uint64_t descriptor, const void* buffer, uint64_t length)
{
    Sync::MutexGuard guard(file_system_lock);
    OpenFile* file = FindOpenFile(table, descriptor);
    if (file == nullptr || !file->writable)
    {
        return User::SyscallErrorBadDescriptor;
    }
    uint64_t bytes_written;
    const bool complete = VFS::Write(&file->handle, buffer, length, &bytes_written);
    // A short write (such as a full disk) still reports the bytes that did land.
    return complete || bytes_written > 0 ? bytes_written : User::SyscallErrorFailed;
}

uint64_t Seek(Table* table, uint64_t descriptor, uint64_t offset)
{
    Sync::MutexGuard guard(file_system_lock);
    OpenFile* file = FindOpenFile(table, descriptor);
    if (file == nullptr)
    {
        return User::SyscallErrorBadDescriptor;
    }
    return VFS::Seek(&file->handle, offset) ? 0 : User::SyscallErrorInvalidArgument;
}

uint64_t Close(Table* table, uint64_t descriptor)
{
    Sync::MutexGuard guard(file_system_lock);
    OpenFile* file = FindOpenFile(table, descriptor);
    if (file == nullptr)
    {
        return User::SyscallErrorBadDescriptor;
    }
    CloseLocked(file);
    return 0;
}

uint64_t Remove(const char* path)
{
    if (path == nullptr)
    {
        return User::SyscallErrorInvalidArgument;
    }
    Sync::MutexGuard guard(file_system_lock);
    VFS::FileHandle handle;
    if (!VFS::Open(path, &handle))
    {
        return VFS::IsDirectory(path) ? User::SyscallErrorIsDirectory : User::SyscallErrorNotFound;
    }
    const bool on_disk = handle.block_backed;
    const bool busy = on_disk && FindUse(UseKey(handle)) != nullptr;
    VFS::Close(&handle);
    if (!on_disk)
    {
        return User::SyscallErrorReadOnly;
    }
    if (busy)
    {
        return User::SyscallErrorBusy;
    }
    return VFS::Remove(path) ? 0 : User::SyscallErrorFailed;
}

uint64_t ReadDirectory(const char* path, uint64_t index, User::DirectoryEntry* entry)
{
    if (path == nullptr || entry == nullptr || index > UINT32_MAX)
    {
        return User::SyscallErrorInvalidArgument;
    }
    Sync::MutexGuard guard(file_system_lock);
    if (!VFS::IsDirectory(path))
    {
        return MissingErrorLocked(path);
    }
    FAT16::DirectoryEntry disk_entry;
    if (!VFS::ReadDirectory(path, static_cast<uint32_t>(index), &disk_entry))
    {
        return User::SyscallErrorNotFound;
    }
    for (uint32_t offset = 0; offset < sizeof(entry->name); ++offset)
    {
        entry->name[offset] = disk_entry.name[offset];
    }
    entry->directory = disk_entry.directory;
    entry->size = disk_entry.size;
    return 0;
}

uint64_t CheckDirectory(const char* path)
{
    if (path == nullptr)
    {
        return User::SyscallErrorInvalidArgument;
    }
    Sync::MutexGuard guard(file_system_lock);
    return VFS::IsDirectory(path) ? 0 : MissingErrorLocked(path);
}

uint64_t MakeDirectory(const char* path)
{
    if (path == nullptr)
    {
        return User::SyscallErrorInvalidArgument;
    }
    Sync::MutexGuard guard(file_system_lock);
    if (ExistsLocked(path))
    {
        return User::SyscallErrorExists;
    }
    return VFS::MakeDirectory(path) ? 0 : CreateErrorLocked(path);
}

uint64_t RemoveDirectory(const char* path)
{
    if (path == nullptr)
    {
        return User::SyscallErrorInvalidArgument;
    }
    Sync::MutexGuard guard(file_system_lock);
    if (!VFS::IsDirectory(path))
    {
        return MissingErrorLocked(path);
    }
    if (VFS::IsReadOnly(path))
    {
        return User::SyscallErrorReadOnly;
    }
    // The root holds the /system mount point, so it can never be removed.
    if (path[0] == '/' && path[1] == '\0')
    {
        return User::SyscallErrorBusy;
    }
    FAT16::DirectoryEntry entry;
    if (VFS::ReadDirectory(path, 0, &entry))
    {
        return User::SyscallErrorNotEmpty;
    }
    return VFS::RemoveDirectory(path) ? 0 : User::SyscallErrorFailed;
}

uint64_t Stat(const char* path, User::FileStatus* status)
{
    if (path == nullptr || status == nullptr)
    {
        return User::SyscallErrorInvalidArgument;
    }
    Sync::MutexGuard guard(file_system_lock);
    status->read_only = VFS::IsReadOnly(path);
    if (VFS::IsDirectory(path))
    {
        status->type = User::FileType::Directory;
        status->size = 0;
        return 0;
    }
    VFS::FileInfo info;
    if (!VFS::Stat(path, &info))
    {
        return User::SyscallErrorNotFound;
    }
    status->type = User::FileType::Regular;
    status->size = info.size;
    return 0;
}

uint64_t StatDescriptor(Table* table, uint64_t descriptor, User::FileStatus* status)
{
    if (status == nullptr)
    {
        return User::SyscallErrorInvalidArgument;
    }
    Sync::MutexGuard guard(file_system_lock);
    const OpenFile* file = FindOpenFile(table, descriptor);
    if (file == nullptr)
    {
        return User::SyscallErrorBadDescriptor;
    }
    status->type = User::FileType::Regular;
    status->size = file->handle.size;
    status->read_only = !file->handle.block_backed;
    return 0;
}

void Quiesce()
{
    file_system_lock.Lock();
}

void CloseAll(Table* table)
{
    if (table == nullptr)
    {
        return;
    }
    Sync::MutexGuard guard(file_system_lock);
    for (OpenFile& file : table->files)
    {
        if (file.used)
        {
            CloseLocked(&file);
        }
    }
}

} // namespace FeatherOS::Files
