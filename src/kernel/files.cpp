#include "files.h"

#include "kernel/memory/heap.h"
#include "kernel/sync/mutex.h"

namespace FeatherOS::Files
{

struct OpenFile
{
    VFS::FileHandle handle;
    Pipes::Pipe* pipe;
    // Descriptors naming this file, in every process.
    uint32_t references;
    Kind kind;
    bool readable;
    bool writable;
    bool append;
};

namespace
{

constexpr uint64_t KnownFlags = User::OpenRead | User::OpenWrite | User::OpenCreate |
                                User::OpenTruncate | User::OpenAppend | User::OpenCloseOnExec;
constexpr uint32_t MaximumOpenDiskFiles = 64;
// FAT16 directory clusters fit in 16 bits, so disk keys never set the top bit.
constexpr uint64_t MemoryKeyBit = 1ULL << 63;

// Disk and /tmp files open anywhere in the system, keyed by where their directory entry or tmpfs
// node is. A VFS handle caches the file's size and first cluster, so a second writer, or removing
// an open file, would act on stale state.
struct DiskFileUse
{
    uint64_t key;
    uint32_t readers;
    bool writer;
};

// Filesystem updates span many sectors; one lock keeps them from interleaving. It also guards
// the descriptor tables and open-file references.
Sync::Mutex file_system_lock;
DiskFileUse disk_files[MaximumOpenDiskFiles];
uint32_t disk_file_count = 0;
// Every process's console descriptors share this one, which is never freed.
OpenFile console = {{}, nullptr, 1, Kind::Console, true, true, false};

uint64_t UseKey(const VFS::FileHandle& handle)
{
    if (handle.kind == VFS::FileKind::Memory)
    {
        return MemoryKeyBit | handle.node;
    }
    return (static_cast<uint64_t>(handle.directory_cluster) << 32) | handle.entry_index;
}

bool IsReadOnlyKind(VFS::FileKind kind)
{
    return kind == VFS::FileKind::Archive || kind == VFS::FileKind::Generated;
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

Slot* FindSlot(Table* table, uint64_t descriptor)
{
    if (table == nullptr || descriptor >= User::MaximumDescriptors ||
        table->slots[descriptor].file == nullptr)
    {
        return nullptr;
    }
    return &table->slots[descriptor];
}

// A file descriptor, not the console.
OpenFile* FindFile(Table* table, uint64_t descriptor)
{
    Slot* slot = FindSlot(table, descriptor);
    return slot == nullptr || slot->file->kind != Kind::File ? nullptr : slot->file;
}

void ReleaseLocked(OpenFile* file)
{
    if (file == &console || --file->references > 0)
    {
        return;
    }
    if (file->kind == Kind::Pipe)
    {
        Pipes::CloseEnd(file->pipe, file->writable);
        Memory::Deallocate(file);
        return;
    }
    if (VFS::IsTracked(file->handle))
    {
        DropUse(UseKey(file->handle), file->writable);
    }
    VFS::Close(&file->handle);
    Memory::Deallocate(file);
}

void CloseSlotLocked(Slot* slot)
{
    ReleaseLocked(slot->file);
    slot->file = nullptr;
    slot->close_on_exec = false;
}

uint64_t FreeSlotLocked(const Table* table, uint64_t minimum)
{
    for (uint64_t descriptor = minimum; descriptor < User::MaximumDescriptors; ++descriptor)
    {
        if (table->slots[descriptor].file == nullptr)
        {
            return descriptor;
        }
    }
    return User::SyscallErrorTooManyOpenFiles;
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

void OpenConsole(Table* table)
{
    Sync::MutexGuard guard(file_system_lock);
    for (uint64_t descriptor = User::StandardInput; descriptor <= User::StandardError; ++descriptor)
    {
        if (table->slots[descriptor].file != nullptr)
        {
            CloseSlotLocked(&table->slots[descriptor]);
        }
        table->slots[descriptor] = {&console, false};
    }
}

void Inherit(Table* child, const Table* parent)
{
    Sync::MutexGuard guard(file_system_lock);
    for (uint64_t descriptor = 0; descriptor < User::MaximumDescriptors; ++descriptor)
    {
        const Slot& slot = parent->slots[descriptor];
        if (slot.file == nullptr || slot.close_on_exec)
        {
            child->slots[descriptor] = {nullptr, false};
            continue;
        }
        ++slot.file->references;
        child->slots[descriptor] = {slot.file, false};
    }
}

Kind KindOf(const Table* table, uint64_t descriptor)
{
    Sync::MutexGuard guard(file_system_lock);
    if (table == nullptr || descriptor >= User::MaximumDescriptors ||
        table->slots[descriptor].file == nullptr)
    {
        return Kind::None;
    }
    return table->slots[descriptor].file->kind;
}

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
    const uint64_t descriptor = FreeSlotLocked(table, 0);
    if (User::IsSyscallError(descriptor))
    {
        return descriptor;
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
    else if (writable && IsReadOnlyKind(handle.kind))
    {
        VFS::Close(&handle);
        return User::SyscallErrorReadOnly;
    }
    auto* file = static_cast<OpenFile*>(Memory::Allocate(sizeof(OpenFile)));
    if (file == nullptr)
    {
        VFS::Close(&handle);
        return User::SyscallErrorNoMemory;
    }
    if (VFS::IsTracked(handle))
    {
        const uint64_t added = AddUse(UseKey(handle), writable);
        if (User::IsSyscallError(added))
        {
            VFS::Close(&handle);
            Memory::Deallocate(file);
            return added;
        }
    }

    file->handle = handle;
    file->pipe = nullptr;
    file->references = 1;
    file->kind = Kind::File;
    file->readable = (flags & User::OpenRead) != 0;
    file->writable = writable;
    file->append = (flags & User::OpenAppend) != 0;
    Slot& slot = table->slots[descriptor];
    slot = {file, (flags & User::OpenCloseOnExec) != 0};
    if ((flags & User::OpenTruncate) != 0 && !VFS::Truncate(&file->handle, 0))
    {
        CloseSlotLocked(&slot);
        return User::SyscallErrorFailed;
    }
    return descriptor;
}

// The pipe stays allocated while the caller waits on it: the caller's own descriptor holds a
// reference, and only the caller can close that.
Pipes::Pipe* FindPipe(Table* table, uint64_t descriptor, bool write_end)
{
    Sync::MutexGuard guard(file_system_lock);
    Slot* slot = FindSlot(table, descriptor);
    if (slot == nullptr || slot->file->kind != Kind::Pipe || slot->file->writable != write_end)
    {
        return nullptr;
    }
    return slot->file->pipe;
}

uint64_t Read(Table* table, uint64_t descriptor, void* buffer, uint64_t length,
              Pipes::InterruptCheck interrupted)
{
    if (KindOf(table, descriptor) == Kind::Pipe)
    {
        Pipes::Pipe* pipe = FindPipe(table, descriptor, false);
        return pipe == nullptr ? User::SyscallErrorBadDescriptor
                               : Pipes::Read(pipe, buffer, length, interrupted);
    }
    Sync::MutexGuard guard(file_system_lock);
    OpenFile* file = FindFile(table, descriptor);
    if (file == nullptr || !file->readable)
    {
        return User::SyscallErrorBadDescriptor;
    }
    uint64_t bytes_read;
    return VFS::Read(&file->handle, buffer, length, &bytes_read) ? bytes_read
                                                                 : User::SyscallErrorFailed;
}

uint64_t Write(Table* table, uint64_t descriptor, const void* buffer, uint64_t length,
               Pipes::InterruptCheck interrupted)
{
    if (KindOf(table, descriptor) == Kind::Pipe)
    {
        Pipes::Pipe* pipe = FindPipe(table, descriptor, true);
        return pipe == nullptr ? User::SyscallErrorBadDescriptor
                               : Pipes::Write(pipe, buffer, length, interrupted);
    }
    Sync::MutexGuard guard(file_system_lock);
    OpenFile* file = FindFile(table, descriptor);
    if (file == nullptr || !file->writable)
    {
        return User::SyscallErrorBadDescriptor;
    }
    // Under the lock, so writes through other descriptors cannot land in between.
    if (file->append)
    {
        VFS::Seek(&file->handle, file->handle.size);
    }
    uint64_t bytes_written;
    const bool complete = VFS::Write(&file->handle, buffer, length, &bytes_written);
    // A short write (such as a full disk) still reports the bytes that did land.
    return complete || bytes_written > 0 ? bytes_written : User::SyscallErrorFailed;
}

// Field by field: assigning the whole open file at once would need memset, which the kernel
// lacks.
void SetPipeEnd(OpenFile* file, Pipes::Pipe* pipe, bool write_end)
{
    file->handle = {};
    file->pipe = pipe;
    file->references = 1;
    file->kind = Kind::Pipe;
    file->readable = !write_end;
    file->writable = write_end;
    file->append = false;
}

uint64_t CreatePipe(Table* table, uint64_t flags, uint64_t* read_end, uint64_t* write_end)
{
    if ((flags & ~User::PipeCloseOnExec) != 0)
    {
        return User::SyscallErrorInvalidArgument;
    }
    Sync::MutexGuard guard(file_system_lock);
    const uint64_t reader = FreeSlotLocked(table, 0);
    const uint64_t writer =
        User::IsSyscallError(reader) ? reader : FreeSlotLocked(table, reader + 1);
    if (User::IsSyscallError(writer))
    {
        return writer;
    }
    auto* reading = static_cast<OpenFile*>(Memory::Allocate(sizeof(OpenFile)));
    auto* writing = static_cast<OpenFile*>(Memory::Allocate(sizeof(OpenFile)));
    Pipes::Pipe* pipe = reading == nullptr || writing == nullptr ? nullptr : Pipes::Create();
    if (pipe == nullptr)
    {
        if (reading != nullptr)
        {
            Memory::Deallocate(reading);
        }
        if (writing != nullptr)
        {
            Memory::Deallocate(writing);
        }
        return User::SyscallErrorNoMemory;
    }
    SetPipeEnd(reading, pipe, false);
    SetPipeEnd(writing, pipe, true);
    const bool close_on_exec = (flags & User::PipeCloseOnExec) != 0;
    table->slots[reader] = {reading, close_on_exec};
    table->slots[writer] = {writing, close_on_exec};
    *read_end = reader;
    *write_end = writer;
    return 0;
}

uint64_t Seek(Table* table, uint64_t descriptor, uint64_t offset, uint64_t whence)
{
    Sync::MutexGuard guard(file_system_lock);
    const Slot* slot = FindSlot(table, descriptor);
    if (slot == nullptr)
    {
        return User::SyscallErrorBadDescriptor;
    }
    if (slot->file->kind != Kind::File)
    {
        return User::SyscallErrorNotSeekable;
    }
    OpenFile* file = slot->file;
    uint64_t origin = 0;
    if (whence == User::SeekFromCurrent)
    {
        origin = file->handle.offset;
    }
    else if (whence == User::SeekFromEnd)
    {
        origin = file->handle.size;
    }
    else if (whence != User::SeekFromStart)
    {
        return User::SyscallErrorInvalidArgument;
    }
    // Unsigned addition wraps like signed addition, so a negative offset moves backward.
    const auto distance = static_cast<int64_t>(offset);
    if (distance < 0 && static_cast<uint64_t>(-(distance + 1)) >= origin)
    {
        return User::SyscallErrorInvalidArgument;
    }
    const uint64_t target = origin + offset;
    return VFS::Seek(&file->handle, target) ? target : User::SyscallErrorInvalidArgument;
}

uint64_t Close(Table* table, uint64_t descriptor)
{
    Sync::MutexGuard guard(file_system_lock);
    Slot* slot = FindSlot(table, descriptor);
    if (slot == nullptr)
    {
        return User::SyscallErrorBadDescriptor;
    }
    CloseSlotLocked(slot);
    return 0;
}

uint64_t Duplicate(Table* table, uint64_t descriptor, uint64_t target, uint64_t flags)
{
    if ((flags & ~(User::DuplicateExact | User::DuplicateCloseOnExec)) != 0 ||
        target >= User::MaximumDescriptors)
    {
        return User::SyscallErrorInvalidArgument;
    }
    Sync::MutexGuard guard(file_system_lock);
    Slot* source = FindSlot(table, descriptor);
    if (source == nullptr)
    {
        return User::SyscallErrorBadDescriptor;
    }
    const bool close_on_exec = (flags & User::DuplicateCloseOnExec) != 0;
    uint64_t result = target;
    if ((flags & User::DuplicateExact) != 0)
    {
        // As in dup2, the same descriptor stays as it is.
        if (target == descriptor)
        {
            return target;
        }
        if (table->slots[target].file != nullptr)
        {
            CloseSlotLocked(&table->slots[target]);
        }
    }
    else
    {
        result = FreeSlotLocked(table, target);
        if (User::IsSyscallError(result))
        {
            return result;
        }
    }
    ++source->file->references;
    table->slots[result] = {source->file, close_on_exec};
    return result;
}

uint64_t DescriptorFlags(Table* table, uint64_t descriptor, bool set, uint64_t value)
{
    if (set && (value & ~User::DescriptorCloseOnExec) != 0)
    {
        return User::SyscallErrorInvalidArgument;
    }
    Sync::MutexGuard guard(file_system_lock);
    Slot* slot = FindSlot(table, descriptor);
    if (slot == nullptr)
    {
        return User::SyscallErrorBadDescriptor;
    }
    if (set)
    {
        slot->close_on_exec = (value & User::DescriptorCloseOnExec) != 0;
    }
    return slot->close_on_exec ? User::DescriptorCloseOnExec : 0;
}

uint64_t CountOpen(const Table* table)
{
    Sync::MutexGuard guard(file_system_lock);
    uint64_t count = 0;
    for (const Slot& slot : table->slots)
    {
        count += slot.file != nullptr ? 1 : 0;
    }
    return count;
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
    const bool removable = VFS::IsTracked(handle);
    const bool busy = removable && FindUse(UseKey(handle)) != nullptr;
    VFS::Close(&handle);
    if (!removable)
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
    if (VFS::IsMountPoint(path))
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
    status->type = info.device ? User::FileType::Device : User::FileType::Regular;
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
    const Slot* slot = FindSlot(table, descriptor);
    if (slot == nullptr)
    {
        return User::SyscallErrorBadDescriptor;
    }
    const OpenFile* file = slot->file;
    if (file->kind == Kind::Console)
    {
        *status = {0, User::FileType::Console, false};
        return 0;
    }
    if (file->kind == Kind::Pipe)
    {
        *status = {Pipes::Buffered(file->pipe), User::FileType::Pipe, !file->writable};
        return 0;
    }
    status->type = file->handle.kind == VFS::FileKind::Device ? User::FileType::Device
                                                              : User::FileType::Regular;
    status->size = file->handle.size;
    status->read_only = IsReadOnlyKind(file->handle.kind);
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
    for (Slot& slot : table->slots)
    {
        if (slot.file != nullptr)
        {
            CloseSlotLocked(&slot);
        }
    }
}

} // namespace FeatherOS::Files
