#include "files.h"

#include "kernel/filesystem/devices.h"
#include "kernel/input/pointer.h"
#include "kernel/memory/heap.h"
#include "kernel/sync/mutex.h"
#include "kernel/terminal/pseudo_terminal.h"

namespace FeatherOS::Files
{

constinit Sync::Mutex table_lock;

namespace
{

// The locks, in the order they are taken. Nothing takes an earlier one while holding a later one.
//
// namespace_lock serializes changes to the mount tree, and the working directories that an unmount
// checks. A task then holds at most one volume lock (Driver::VolumeLock; Quiesce is the exception).
// table_lock guards the descriptor tables, the open files' references and disk_files, and is held
// across no driver call and no wait for a volume lock. mount_lock is the VFS's. table_lock is also
// taken before process_lock: closing a pty master under it hangs up a session, which takes
// process_lock, and /proc's process status counts a table with both.
constinit Sync::Mutex namespace_lock;
// The role of a task that holds the volume lock of the file it works on, which guards the handle.
[[maybe_unused]] constinit Sync::Role held_volume;

#ifdef FEATHEROS_HOST_TEST
Testing::PauseHook pause_hook = nullptr;

void Pause(const char* point)
{
    if (pause_hook != nullptr)
    {
        pause_hook(point);
    }
}
#else
inline void Pause(const char* /*point*/) {}
#endif

} // namespace

struct OpenFile
{
    VFS::FileHandle handle GUARDED_BY(held_volume);
    Pipes::Pipe* pipe;
    // Descriptors naming this file, in every process, and tasks using it without table_lock.
    uint32_t references GUARDED_BY(table_lock);
    Kind kind;
    bool readable;
    bool writable;
    bool append;
    bool nonblocking;
    Network::Sockets::Socket* socket;
    Terminals::Terminal* terminal;
    // A master's pair, or a slave's, which the descriptor holds open.
    PseudoTerminals::Pair* pair;
    // A PointerEvents file's cursor; Input::Pointer serializes its use.
    Input::Pointer::Reader pointer;
};

namespace
{

constexpr uint64_t KnownFlags = User::OpenRead | User::OpenWrite | User::OpenCreate |
                                User::OpenTruncate | User::OpenAppend | User::OpenCloseOnExec |
                                User::OpenNonBlocking | User::OpenNoControllingTerminal |
                                User::OpenNoFollow;
constexpr uint32_t InitialOpenDiskFiles = 64;
constexpr uint32_t MaximumOpenDiskFiles = 1024;
constexpr uint32_t InitialDescriptors = 32;
// Linux's permissions for terminals, pipes and sockets.
constexpr uint32_t TerminalMode = 0620;
constexpr uint32_t PipeMode = 0600;
constexpr uint32_t PointerMode = 0666;
constexpr uint32_t SocketMode = 0777;
constexpr uint32_t FramebufferMode = 0660;

// Disk, /tmp and /etc files open anywhere in the system, keyed by their filesystem and node. A VFS
// handle caches the file's size and where its data starts, so a second writer, or removing an
// open file, would act on stale state.
struct DiskFileUse
{
    VFS::FileKey key;
    uint32_t readers;
    bool writer;
};

// A heap array that doubles from InitialOpenDiskFiles to MaximumOpenDiskFiles; nothing holds a
// pointer into it across a call that adds an entry.
DiskFileUse* disk_files GUARDED_BY(table_lock) = nullptr;
uint32_t disk_file_capacity GUARDED_BY(table_lock) = 0;
uint32_t disk_file_count GUARDED_BY(table_lock) = 0;
FramebufferRelease framebuffer_release GUARDED_BY(table_lock) = nullptr;
// Every process's console descriptors share this one, which is never freed.
OpenFile console = {{},    nullptr, 1,       Kind::Terminal, true,    true,
                    false, false,   nullptr, nullptr,        nullptr, {}};

DiskFileUse* FindUse(const VFS::FileKey& key) REQUIRES(table_lock)
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

uint64_t GrowDiskFilesLocked() REQUIRES(table_lock)
{
    const uint32_t capacity =
        disk_file_capacity == 0 ? InitialOpenDiskFiles : disk_file_capacity * 2;
    auto* grown = static_cast<DiskFileUse*>(Memory::AllocateZeroed(capacity, sizeof(DiskFileUse)));
    if (grown == nullptr)
    {
        return User::SyscallErrorNoMemory;
    }
    for (uint32_t index = 0; index < disk_file_count; ++index)
    {
        grown[index] = disk_files[index];
    }
    if (disk_files != nullptr)
    {
        Memory::Deallocate(disk_files);
    }
    disk_files = grown;
    disk_file_capacity = capacity;
    return 0;
}

// Room for one more distinct file, so an open can check before it creates anything.
uint64_t ReserveUseLocked() REQUIRES(table_lock)
{
    if (disk_file_count == MaximumOpenDiskFiles)
    {
        return User::SyscallErrorTooManyFilesInSystem;
    }
    return disk_file_count == disk_file_capacity ? GrowDiskFilesLocked() : 0;
}

uint64_t AddUse(const VFS::FileKey& key, bool writable) REQUIRES(table_lock)
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
    const uint64_t reserved = ReserveUseLocked();
    if (reserved != 0)
    {
        return reserved;
    }
    disk_files[disk_file_count++] = {key, writable ? 0U : 1U, writable};
    return 0;
}

void DropUse(const VFS::FileKey& key, bool writable) REQUIRES(table_lock)
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

Slot* FindSlot(Table* table, uint64_t descriptor) REQUIRES(table_lock)
{
    if (table == nullptr || descriptor >= table->capacity ||
        table->slots[descriptor].file == nullptr)
    {
        return nullptr;
    }
    return &table->slots[descriptor];
}

// A file descriptor, not the console.
OpenFile* FindFile(Table* table, uint64_t descriptor) REQUIRES(table_lock)
{
    Slot* slot = FindSlot(table, descriptor);
    return slot == nullptr || slot->file->kind != Kind::File ? nullptr : slot->file;
}

bool InUse(const VFS::FileKey& key)
{
    Sync::MutexGuard guard(table_lock);
    return FindUse(key) != nullptr;
}

// An open file's driver never changes, so it can be read to find the volume lock the handle needs.
Filesystem::Driver* DriverOf(const OpenFile* file) NO_THREAD_SAFETY_ANALYSIS
{
    return file->handle.driver;
}

// Drops one reference. Files other than disk files close here; the last reference to a disk file
// hands it back, for FinishClose to close outside table_lock under its volume lock.
OpenFile* DropReferenceLocked(OpenFile* file) REQUIRES(table_lock)
{
    if (file == &console || --file->references > 0)
    {
        return nullptr;
    }
    if (file->kind == Kind::Pipe)
    {
        Pipes::CloseEnd(file->pipe, file->writable);
        Memory::Deallocate(file);
        return nullptr;
    }
    if (file->kind == Kind::Framebuffer)
    {
        if (framebuffer_release != nullptr)
        {
            framebuffer_release(file);
        }
        Memory::Deallocate(file);
        return nullptr;
    }
    if (file->kind == Kind::PointerEvents)
    {
        Memory::Deallocate(file);
        return nullptr;
    }
    if (file->kind == Kind::Socket)
    {
        Network::Sockets::Close(file->socket);
        Memory::Deallocate(file);
        return nullptr;
    }
    if (file->kind == Kind::Terminal || file->kind == Kind::PseudoTerminalMaster)
    {
        if (file->kind == Kind::PseudoTerminalMaster)
        {
            PseudoTerminals::CloseMaster(file->pair);
        }
        else if (file->pair != nullptr)
        {
            PseudoTerminals::CloseSlave(file->pair);
        }
        Memory::Deallocate(file);
        return nullptr;
    }
    return file;
}

OpenFile* DetachSlotLocked(Slot* slot) REQUIRES(table_lock)
{
    OpenFile* const file = slot->file;
    slot->file = nullptr;
    slot->close_on_exec = false;
    return DropReferenceLocked(file);
}

// Takes the file's volume lock, then table_lock to forget its use, as every closer does.
void FinishCloseHeld(OpenFile* file) REQUIRES(held_volume) EXCLUDES(table_lock)
{
    {
        Sync::MutexGuard guard(table_lock);
        if (VFS::IsTracked(file->handle))
        {
            DropUse(VFS::KeyOf(file->handle), file->writable);
        }
    }
    VFS::Close(&file->handle);
    Memory::Deallocate(file);
}

Sync::VolumeMutex* LockOf(Filesystem::Driver* driver)
{
    return driver != nullptr ? &driver->VolumeLock() : nullptr;
}

// Keeps a mount from being unmounted while its holder works on it; destroyed last.
class MountUser
{
protected:
    explicit MountUser(Filesystem::Driver* driver) : driver(driver) {}
    ~MountUser()
    {
        VFS::ReleaseMount(driver);
    }
    MountUser(const MountUser&) = delete;
    MountUser& operator=(const MountUser&) = delete;

    Filesystem::Driver* const driver;
};

// Holds one volume lock, unless the task has it, for a sequence of calls that must not be
// interleaved with another task's on the same volume.
class SCOPED_CAPABILITY VolumeHold : private MountUser, private Sync::VolumeHoldBase
{
public:
    // The mount holding path, which may be null for none; the nested calls name paths on it.
    explicit VolumeHold(const char* path) ACQUIRE(held_volume) NO_THREAD_SAFETY_ANALYSIS
        : MountUser(VFS::AcquireMount(path)),
          Sync::VolumeHoldBase(LockOf(driver))
    {
    }
    // A driver whose open file the caller keeps, which holds its mount.
    explicit VolumeHold(Filesystem::Driver* target) ACQUIRE(held_volume) NO_THREAD_SAFETY_ANALYSIS
        : MountUser(nullptr),
          Sync::VolumeHoldBase(LockOf(target))
    {
    }
    ~VolumeHold() RELEASE() NO_THREAD_SAFETY_ANALYSIS = default;
};

void FinishClose(OpenFile* file) EXCLUDES(held_volume, table_lock)
{
    if (file != nullptr)
    {
        const VolumeHold hold(DriverOf(file));
        FinishCloseHeld(file);
    }
}

void Unpin(OpenFile* file) EXCLUDES(held_volume, table_lock)
{
    OpenFile* last = nullptr;
    {
        Sync::MutexGuard guard(table_lock);
        last = DropReferenceLocked(file);
    }
    FinishClose(last);
}

bool Inheritable(const Slot& slot)
{
    return slot.file != nullptr && !slot.close_on_exec && slot.file->kind != Kind::Framebuffer;
}

// The array grows to the next power of two that holds needed slots, but not past the soft limit;
// callers check needed against it first. Slot pointers into the table do not survive this.
uint64_t GrowLocked(Table* table, uint64_t needed) REQUIRES(table_lock)
{
    if (needed <= table->capacity)
    {
        return 0;
    }
    uint32_t size = InitialDescriptors;
    while (size < needed)
    {
        size *= 2;
    }
    if (size > table->soft_limit)
    {
        size = table->soft_limit;
    }
    auto* grown = static_cast<Slot*>(Memory::AllocateZeroed(size, sizeof(Slot)));
    if (grown == nullptr)
    {
        return User::SyscallErrorNoMemory;
    }
    for (uint32_t index = 0; index < table->capacity; ++index)
    {
        grown[index] = table->slots[index];
    }
    if (table->slots != nullptr)
    {
        Memory::Deallocate(table->slots);
    }
    table->slots = grown;
    table->capacity = size;
    return 0;
}

// The lowest free descriptor at or above minimum and below the soft limit, growing the table when
// none is free; the result stays free until the caller fills it.
uint64_t FreeSlotLocked(Table* table, uint64_t minimum) REQUIRES(table_lock)
{
    if (minimum >= table->soft_limit)
    {
        return User::SyscallErrorTooManyOpenFiles;
    }
    const uint64_t end = table->capacity < table->soft_limit ? table->capacity : table->soft_limit;
    for (uint64_t descriptor = minimum; descriptor < end; ++descriptor)
    {
        if (table->slots[descriptor].file == nullptr)
        {
            return descriptor;
        }
    }
    if (table->capacity >= table->soft_limit)
    {
        return User::SyscallErrorTooManyOpenFiles;
    }
    const uint64_t descriptor = minimum > table->capacity ? minimum : table->capacity;
    const uint64_t grown = GrowLocked(table, descriptor + 1);
    return grown == 0 ? descriptor : grown;
}

// Anything at path, a link included.
bool Exists(const char* path)
{
    VFS::FileInfo info;
    return VFS::Stat(path, &info);
}

bool IsLink(const char* path)
{
    VFS::FileInfo info;
    return VFS::Stat(path, &info) && (info.mode & User::ModeTypeMask) == User::ModeSymbolicLink;
}

uint64_t WalkError(VFS::WalkResult result)
{
    switch (result)
    {
    case VFS::WalkResult::Done:
        return 0;
    case VFS::WalkResult::NotFound:
        return User::SyscallErrorNotFound;
    case VFS::WalkResult::NotDirectory:
        return User::SyscallErrorNotDirectory;
    case VFS::WalkResult::Loop:
        return User::SyscallErrorLoop;
    case VFS::WalkResult::NameTooLong:
        break;
    }
    return User::SyscallErrorNameTooLong;
}

// A path walked into its physical form, which the VFS calls take; its buffers are on the heap.
class Resolved
{
public:
    Resolved() = default;
    ~Resolved() = default;
    Resolved(const Resolved&) = delete;
    Resolved& operator=(const Resolved&) = delete;
    Resolved(Resolved&&) = delete;
    Resolved& operator=(Resolved&&) = delete;

    uint64_t Walk(const char* path, VFS::Follow follow) EXCLUDES(held_volume)
    {
        if (!walk)
        {
            return User::SyscallErrorNoMemory;
        }
        return WalkError(VFS::ResolvePath(path, follow, &*walk));
    }

    const char* Path() const
    {
        return walk->path;
    }

private:
    Memory::HeapObject<VFS::PathWalk> walk;
};

struct PathText
{
    char text[User::MaximumPathLength];
};

// Paths reach Files already resolved to absolute form, so the parent is everything before the
// last '/'.
bool ParentExists(const char* path)
{
    const Memory::HeapObject<PathText> parent;
    if (!parent)
    {
        return false;
    }
    uint64_t last_separator = 0;
    uint64_t length = 0;
    for (; path[length] != '\0' && length < sizeof(parent->text) - 1; ++length)
    {
        parent->text[length] = path[length];
        if (path[length] == '/')
        {
            last_separator = length;
        }
    }
    parent->text[last_separator == 0 ? 1 : last_separator] = '\0';
    return VFS::IsDirectory(parent->text);
}

User::FileTime ToFileTime(const Filesystem::Timestamp& time)
{
    return {time.seconds, time.nanoseconds, 0};
}

// 0 is an ext2 special node whose kind the shared core does not tell.
User::FileType KindOfMode(uint32_t mode)
{
    switch (mode & User::ModeTypeMask)
    {
    case User::ModeRegular:
        return User::FileType::Regular;
    case User::ModeDirectory:
        return User::FileType::Directory;
    case User::ModeCharacterDevice:
    case User::ModeBlockDevice:
        return User::FileType::Device;
    case User::ModeFifo:
        return User::FileType::Pipe;
    case User::ModeSocket:
        return User::FileType::Socket;
    case User::ModeSymbolicLink:
        return User::FileType::SymbolicLink;
    default:
        return User::FileType::Unknown;
    }
}

User::FileStatus ToFileStatus(const VFS::FileInfo& info, bool read_only)
{
    return {
        .device = info.device,
        .node = info.node,
        .mode = info.mode,
        .link_count = info.link_count,
        .owner = info.owner,
        .group = info.group,
        .size = info.size,
        .access_time = ToFileTime(info.access_time),
        .modify_time = ToFileTime(info.modify_time),
        .change_time = ToFileTime(info.change_time),
        .type = KindOfMode(info.mode),
        .read_only = read_only,
    };
}

// Pipes, sockets and terminals live outside the mounts; their address names them.
User::FileStatus UnmountedStatus(const void* object, uint32_t mode, User::FileType type,
                                 uint64_t size, bool read_only)
{
    User::FileStatus status = {};
    status.node = reinterpret_cast<uintptr_t>(object);
    status.mode = mode;
    status.link_count = 1;
    status.size = size;
    status.type = type;
    status.read_only = read_only;
    return status;
}

// A volume whose medium is gone answers EIO where it would say nothing was found.
uint64_t NotFoundError(const char* path)
{
    return VFS::IsDead(path) ? User::SyscallErrorInputOutput : User::SyscallErrorNotFound;
}

// Why an operation that changes path could not create it.
uint64_t CreateError(const char* path)
{
    if (VFS::IsDead(path))
    {
        return User::SyscallErrorInputOutput;
    }
    if (Exists(path))
    {
        return VFS::IsDirectory(path) ? User::SyscallErrorIsDirectory : User::SyscallErrorExists;
    }
    if (VFS::IsReadOnly(path))
    {
        return User::SyscallErrorReadOnly;
    }
    return ParentExists(path) ? VFS::LastError() : User::SyscallErrorNotFound;
}

enum class LastName : uint8_t
{
    Plain,
    Dot,
    DotDot,
};

// The last component of path as the caller wrote it, before resolving drops "." and "..".
LastName LastNameOf(const char* path)
{
    uint64_t end = 0;
    while (path[end] != '\0')
    {
        ++end;
    }
    while (end > 0 && path[end - 1] == '/')
    {
        --end;
    }
    uint64_t start = end;
    while (start > 0 && path[start - 1] != '/')
    {
        --start;
    }
    if (end - start == 1 && path[start] == '.')
    {
        return LastName::Dot;
    }
    return end - start == 2 && path[start] == '.' && path[start + 1] == '.' ? LastName::DotDot
                                                                            : LastName::Plain;
}

uint64_t MissingError(const char* path)
{
    return Exists(path) ? User::SyscallErrorNotDirectory : User::SyscallErrorNotFound;
}

uint64_t MountError(VFS::MountResult result)
{
    switch (result)
    {
    case VFS::MountResult::Done:
        return 0;
    case VFS::MountResult::NotFound:
        return User::SyscallErrorNotFound;
    case VFS::MountResult::Busy:
        return User::SyscallErrorBusy;
    case VFS::MountResult::UnknownType:
        return User::SyscallErrorUnknownFilesystemType;
    case VFS::MountResult::Full:
        return User::SyscallErrorNoMemory;
    case VFS::MountResult::Failed:
        return User::SyscallErrorInputOutput;
    case VFS::MountResult::ReadOnly:
        return User::SyscallErrorReadOnly;
    case VFS::MountResult::NoDevice:
        return User::SyscallErrorNoDevice;
    case VFS::MountResult::Invalid:
        break;
    }
    return User::SyscallErrorInvalidArgument;
}

// The use key of the disk or /tmp file at path; false for anything else.
bool FileKeyOf(const char* path, VFS::FileKey* key)
{
    VFS::FileHandle handle;
    if (!VFS::Open(path, &handle))
    {
        return false;
    }
    const bool tracked = VFS::IsTracked(handle);
    *key = VFS::KeyOf(handle);
    VFS::Close(&handle);
    return tracked;
}

// By characters alone, which is enough for resolved paths.
bool IsWithin(const char* path, const char* base)
{
    uint64_t length = 0;
    while (base[length] != '\0' && path[length] == base[length])
    {
        ++length;
    }
    return base[length] == '\0' && (path[length] == '\0' || path[length] == '/');
}

// A file nothing else sees yet, with one reference; the caller sets the rest.
OpenFile* NewFile(Kind kind, bool readable, bool writable) NO_THREAD_SAFETY_ANALYSIS
{
    auto* file = static_cast<OpenFile*>(Memory::Allocate(sizeof(OpenFile)));
    if (file != nullptr)
    {
        *file = {
            .handle = {},
            .pipe = nullptr,
            .references = 1,
            .kind = kind,
            .readable = readable,
            .writable = writable,
            .append = false,
            .nonblocking = false,
            .socket = nullptr,
            .terminal = nullptr,
            .pair = nullptr,
            .pointer = {},
        };
    }
    return file;
}

// A slave's pair stays in use until the descriptor is closed, so terminal stays valid.
uint64_t AddTerminalLocked(Table* table, uint64_t flags, Kind kind, Terminals::Terminal* terminal,
                           PseudoTerminals::Pair* pair) REQUIRES(table_lock)
{
    const uint64_t descriptor = FreeSlotLocked(table, 0);
    if (User::IsSyscallError(descriptor))
    {
        return descriptor;
    }
    OpenFile* const file =
        NewFile(kind, (flags & User::OpenRead) != 0, (flags & User::OpenWrite) != 0);
    if (file == nullptr)
    {
        return User::SyscallErrorNoMemory;
    }
    file->nonblocking = (kind == Kind::PseudoTerminalMaster || kind == Kind::Framebuffer) &&
                        (flags & User::OpenNonBlocking) != 0;
    file->terminal = terminal;
    file->pair = pair;
    table->slots[descriptor] = {file, (flags & User::OpenCloseOnExec) != 0};
    return descriptor;
}

uint64_t AddPointerLocked(Table* table, uint64_t flags) REQUIRES(table_lock)
{
    const uint64_t descriptor = FreeSlotLocked(table, 0);
    if (User::IsSyscallError(descriptor))
    {
        return descriptor;
    }
    OpenFile* const file =
        NewFile(Kind::PointerEvents, (flags & User::OpenRead) != 0, (flags & User::OpenWrite) != 0);
    if (file == nullptr)
    {
        return User::SyscallErrorNoMemory;
    }
    file->nonblocking = (flags & User::OpenNonBlocking) != 0;
    file->pointer = Input::Pointer::Open();
    table->slots[descriptor] = {file, (flags & User::OpenCloseOnExec) != 0};
    return descriptor;
}

uint64_t OpenDeviceLocked(Table* table, uint64_t flags, uint32_t node,
                          Terminals::Terminal* controlling_terminal) REQUIRES(table_lock)
{
    if (Devices::DeviceOf(node) == Devices::Device::Mouse)
    {
        return AddPointerLocked(table, flags);
    }
    PseudoTerminals::Pair* pair = nullptr;
    Terminals::Terminal* terminal = controlling_terminal;
    Kind kind = Kind::Terminal;
    switch (Devices::DeviceOf(node))
    {
    case Devices::Device::ControllingTerminal:
        if (terminal == nullptr)
        {
            return User::SyscallErrorNoDevice;
        }
        pair = PseudoTerminals::AddSlave(terminal);
        break;
    case Devices::Device::PseudoTerminalMaster:
    {
        const uint64_t created = PseudoTerminals::Create(&pair);
        if (created != 0)
        {
            return created;
        }
        terminal = nullptr;
        kind = Kind::PseudoTerminalMaster;
        break;
    }
    case Devices::Device::PseudoTerminalSlave:
    {
        const uint64_t opened = PseudoTerminals::OpenSlave(Devices::PairOf(node), &pair);
        if (opened != 0)
        {
            return opened;
        }
        terminal = PseudoTerminals::TerminalOf(pair);
        break;
    }
    case Devices::Device::Console:
        terminal = Devices::ConsoleOf(node);
        break;
    case Devices::Device::Framebuffer:
        terminal = nullptr;
        kind = Kind::Framebuffer;
        flags |= User::OpenCloseOnExec;
        break;
    default:
        return User::SyscallErrorNotFound;
    }
    const uint64_t added = AddTerminalLocked(table, flags, kind, terminal, pair);
    if (User::IsSyscallError(added) && pair != nullptr)
    {
        if (kind == Kind::PseudoTerminalMaster)
        {
            PseudoTerminals::CloseMaster(pair);
        }
        else
        {
            PseudoTerminals::CloseSlave(pair);
        }
    }
    return added;
}

uint64_t StatPath(const char* path, User::FileStatus* status, VFS::Follow follow)
{
    if (path == nullptr || status == nullptr)
    {
        return User::SyscallErrorInvalidArgument;
    }
    Resolved resolved;
    const uint64_t walked = resolved.Walk(path, follow);
    if (walked != 0)
    {
        return walked;
    }
    const VolumeHold hold(resolved.Path());
    VFS::FileInfo info;
    if (!VFS::Stat(resolved.Path(), &info))
    {
        return NotFoundError(resolved.Path());
    }
    *status = ToFileStatus(info, VFS::IsReadOnly(resolved.Path()));
    return 0;
}

// Why a new link cannot be made at path, in Linux's order, or 0.
uint64_t NewNameError(const char* path, Filesystem::LinkKind kind)
{
    if (Exists(path))
    {
        return User::SyscallErrorExists;
    }
    if (!ParentExists(path))
    {
        return User::SyscallErrorNotFound;
    }
    if (VFS::IsReadOnly(path))
    {
        return User::SyscallErrorReadOnly;
    }
    return VFS::SupportsLinks(path, kind) ? 0 : User::SyscallErrorNotPermitted;
}

bool IsSpecialDevice(const VFS::FileHandle& handle)
{
    uint32_t node;
    if (!VFS::DeviceNode(handle, &node))
    {
        return false;
    }
    const Devices::Device device = Devices::DeviceOf(node);
    return device == Devices::Device::ControllingTerminal ||
           device == Devices::Device::PseudoTerminalMaster ||
           device == Devices::Device::PseudoTerminalSlave || device == Devices::Device::Console ||
           device == Devices::Device::Framebuffer || device == Devices::Device::Mouse;
}

} // namespace

bool OpenConsole(Table* table, Terminals::Terminal* terminal)
{
    constexpr uint64_t Standard = User::StandardError - User::StandardInput + 1;
    OpenFile* closing[Standard] = {};
    {
        Sync::MutexGuard guard(table_lock);
        console.terminal = terminal;
        table->soft_limit = User::DefaultDescriptorLimit;
        table->hard_limit = User::MaximumDescriptorLimit;
        if (GrowLocked(table, User::StandardError + 1) != 0)
        {
            return false;
        }
        for (uint64_t descriptor = User::StandardInput; descriptor <= User::StandardError;
             ++descriptor)
        {
            if (table->slots[descriptor].file != nullptr)
            {
                closing[descriptor - User::StandardInput] =
                    DetachSlotLocked(&table->slots[descriptor]);
            }
            table->slots[descriptor] = {&console, false};
        }
    }
    for (OpenFile* file : closing)
    {
        FinishClose(file);
    }
    return true;
}

bool Inherit(Table* child, const Table* parent)
{
    Sync::MutexGuard guard(table_lock);
    child->soft_limit = parent->soft_limit;
    child->hard_limit = parent->hard_limit;
    uint32_t span = 0;
    for (uint32_t descriptor = 0; descriptor < parent->capacity; ++descriptor)
    {
        if (Inheritable(parent->slots[descriptor]))
        {
            span = descriptor + 1;
        }
    }
    if (span == 0)
    {
        return true;
    }
    uint32_t size = InitialDescriptors;
    while (size < span)
    {
        size *= 2;
    }
    auto* slots = static_cast<Slot*>(Memory::AllocateZeroed(size, sizeof(Slot)));
    if (slots == nullptr)
    {
        return false;
    }
    for (uint32_t descriptor = 0; descriptor < span; ++descriptor)
    {
        const Slot& slot = parent->slots[descriptor];
        if (Inheritable(slot))
        {
            ++slot.file->references;
            slots[descriptor] = {slot.file, false};
        }
    }
    child->slots = slots;
    child->capacity = size;
    return true;
}

uint64_t DescriptorLimits(Table* table, const User::ResourceLimits* requested,
                          User::ResourceLimits* previous)
{
    Sync::MutexGuard guard(table_lock);
    if (previous != nullptr)
    {
        *previous = {table->soft_limit, table->hard_limit};
    }
    if (requested == nullptr)
    {
        return 0;
    }
    if (requested->current > requested->maximum)
    {
        return User::SyscallErrorInvalidArgument;
    }
    if (requested->maximum > User::MaximumDescriptorLimit)
    {
        return User::SyscallErrorNotPermitted;
    }
    table->soft_limit = static_cast<uint32_t>(requested->current);
    table->hard_limit = static_cast<uint32_t>(requested->maximum);
    return 0;
}

Kind KindOf(const Table* table, uint64_t descriptor)
{
    Sync::MutexGuard guard(table_lock);
    if (table == nullptr || descriptor >= table->capacity ||
        table->slots[descriptor].file == nullptr)
    {
        return Kind::None;
    }
    return table->slots[descriptor].file->kind;
}

uint64_t Open(Table* table, const char* path, uint64_t flags,
              Terminals::Terminal* controlling_terminal)
{
    const bool writable = (flags & User::OpenWrite) != 0;
    if (table == nullptr || path == nullptr || (flags & ~KnownFlags) != 0 ||
        (flags & (User::OpenRead | User::OpenWrite)) == 0 ||
        ((flags & (User::OpenCreate | User::OpenTruncate | User::OpenAppend)) != 0 && !writable))
    {
        return User::SyscallErrorInvalidArgument;
    }

    {
        Sync::MutexGuard guard(table_lock);
        const uint64_t free_descriptor = FreeSlotLocked(table, 0);
        if (User::IsSyscallError(free_descriptor))
        {
            return free_descriptor;
        }
    }
    const bool no_follow = (flags & User::OpenNoFollow) != 0;
    Resolved resolved;
    const uint64_t walked =
        resolved.Walk(path, no_follow ? VFS::Follow::NotLast : VFS::Follow::All);
    if (walked != 0)
    {
        return walked;
    }
    path = resolved.Path();
    const VolumeHold hold(path);
    if (no_follow && IsLink(path))
    {
        return User::SyscallErrorLoop;
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
            return NotFoundError(path);
        }
        uint64_t reserved = 0;
        {
            Sync::MutexGuard guard(table_lock);
            reserved = ReserveUseLocked();
        }
        if (reserved != 0)
        {
            return reserved;
        }
        if (!VFS::Create(path, &handle))
        {
            return CreateError(path);
        }
    }
    else if (writable && !VFS::IsWritable(handle))
    {
        VFS::Close(&handle);
        return User::SyscallErrorReadOnly;
    }
    Pause("open.created");
    uint32_t node;
    if (IsSpecialDevice(handle) && VFS::DeviceNode(handle, &node))
    {
        VFS::Close(&handle);
        Sync::MutexGuard guard(table_lock);
        return OpenDeviceLocked(table, flags, node, controlling_terminal);
    }
    OpenFile* const file = NewFile(Kind::File, (flags & User::OpenRead) != 0, writable);
    if (file == nullptr)
    {
        VFS::Close(&handle);
        return User::SyscallErrorNoMemory;
    }
    file->handle = handle;
    file->append = (flags & User::OpenAppend) != 0;
    file->nonblocking = (flags & User::OpenNonBlocking) != 0;
    uint64_t descriptor = 0;
    {
        Sync::MutexGuard guard(table_lock);
        descriptor = FreeSlotLocked(table, 0);
        if (!User::IsSyscallError(descriptor) && VFS::IsTracked(handle))
        {
            const uint64_t added = AddUse(VFS::KeyOf(handle), writable);
            descriptor = User::IsSyscallError(added) ? added : descriptor;
        }
        if (!User::IsSyscallError(descriptor))
        {
            table->slots[descriptor] = {file, (flags & User::OpenCloseOnExec) != 0};
        }
    }
    if (User::IsSyscallError(descriptor))
    {
        VFS::Close(&file->handle);
        Memory::Deallocate(file);
        return descriptor;
    }
    if ((flags & User::OpenTruncate) != 0 && !VFS::Truncate(&file->handle, 0))
    {
        const uint64_t error = VFS::LastError();
        OpenFile* closing = nullptr;
        {
            Sync::MutexGuard guard(table_lock);
            Slot& slot = table->slots[descriptor];
            closing = slot.file == file ? DetachSlotLocked(&slot) : nullptr;
        }
        if (closing != nullptr)
        {
            FinishCloseHeld(closing);
        }
        return error;
    }
    return descriptor;
}

// The pipe stays allocated while the caller waits on it: the caller's own descriptor holds a
// reference, and only the caller can close that.
Pipes::Pipe* FindPipe(Table* table, uint64_t descriptor, bool write_end, bool* nonblocking)
{
    Sync::MutexGuard guard(table_lock);
    Slot* slot = FindSlot(table, descriptor);
    if (slot == nullptr || slot->file->kind != Kind::Pipe || slot->file->writable != write_end)
    {
        return nullptr;
    }
    *nonblocking = slot->file->nonblocking;
    return slot->file->pipe;
}

PseudoTerminals::Pair* FindMaster(Table* table, uint64_t descriptor, bool write, bool* nonblocking)
{
    Sync::MutexGuard guard(table_lock);
    Slot* slot = FindSlot(table, descriptor);
    if (slot == nullptr || slot->file->kind != Kind::PseudoTerminalMaster ||
        !(write ? slot->file->writable : slot->file->readable))
    {
        return nullptr;
    }
    *nonblocking = slot->file->nonblocking;
    return slot->file->pair;
}

// The file stays pinned while the reader waits for events.
uint64_t ReadPointer(Table* table, uint64_t descriptor, void* buffer, uint64_t length,
                     Pipes::InterruptCheck interrupted)
{
    OpenFile* file = nullptr;
    bool nonblocking = false;
    {
        Sync::MutexGuard guard(table_lock);
        Slot* slot = FindSlot(table, descriptor);
        if (slot == nullptr || slot->file->kind != Kind::PointerEvents || !slot->file->readable)
        {
            return User::SyscallErrorBadDescriptor;
        }
        file = slot->file;
        nonblocking = file->nonblocking;
        ++file->references;
    }
    const uint64_t result =
        Input::Pointer::Read(file->pointer, buffer, length, nonblocking, interrupted);
    Unpin(file);
    return result;
}

uint64_t Read(Table* table, uint64_t descriptor, void* buffer, uint64_t length,
              Pipes::InterruptCheck interrupted)
{
    const Kind kind = KindOf(table, descriptor);
    if (kind == Kind::Framebuffer)
    {
        return User::SyscallErrorInvalidArgument;
    }
    if (kind == Kind::PseudoTerminalMaster)
    {
        bool nonblocking = false;
        PseudoTerminals::Pair* pair = FindMaster(table, descriptor, false, &nonblocking);
        return pair == nullptr
                   ? User::SyscallErrorBadDescriptor
                   : PseudoTerminals::ReadMaster(pair, buffer, length, interrupted, nonblocking);
    }
    if (kind == Kind::PointerEvents)
    {
        return ReadPointer(table, descriptor, buffer, length, interrupted);
    }
    if (kind == Kind::Pipe)
    {
        bool nonblocking = false;
        Pipes::Pipe* pipe = FindPipe(table, descriptor, false, &nonblocking);
        return pipe == nullptr ? User::SyscallErrorBadDescriptor
                               : Pipes::Read(pipe, buffer, length, interrupted, nonblocking);
    }
    OpenFile* file = nullptr;
    {
        Sync::MutexGuard guard(table_lock);
        file = FindFile(table, descriptor);
        if (file == nullptr || !file->readable)
        {
            return User::SyscallErrorBadDescriptor;
        }
        ++file->references;
    }
    Pause("read.pinned");
    uint64_t result = User::SyscallErrorFailed;
    {
        const VolumeHold hold(DriverOf(file));
        uint64_t bytes_read;
        if (VFS::Read(&file->handle, buffer, length, &bytes_read))
        {
            result = bytes_read;
        }
    }
    Unpin(file);
    return result;
}

uint64_t Write(Table* table, uint64_t descriptor, const void* buffer, uint64_t length,
               Pipes::InterruptCheck interrupted)
{
    const Kind kind = KindOf(table, descriptor);
    if (kind == Kind::Framebuffer)
    {
        return User::SyscallErrorInvalidArgument;
    }
    if (kind == Kind::PseudoTerminalMaster)
    {
        bool nonblocking = false;
        PseudoTerminals::Pair* pair = FindMaster(table, descriptor, true, &nonblocking);
        return pair == nullptr
                   ? User::SyscallErrorBadDescriptor
                   : PseudoTerminals::WriteMaster(pair, buffer, length, interrupted, nonblocking);
    }
    if (kind == Kind::PointerEvents)
    {
        Sync::MutexGuard guard(table_lock);
        const Slot* slot = FindSlot(table, descriptor);
        return slot != nullptr && slot->file->kind == Kind::PointerEvents && slot->file->writable
                   ? length
                   : User::SyscallErrorBadDescriptor;
    }
    if (kind == Kind::Pipe)
    {
        bool nonblocking = false;
        Pipes::Pipe* pipe = FindPipe(table, descriptor, true, &nonblocking);
        return pipe == nullptr ? User::SyscallErrorBadDescriptor
                               : Pipes::Write(pipe, buffer, length, interrupted, nonblocking);
    }
    OpenFile* file = nullptr;
    bool append = false;
    {
        Sync::MutexGuard guard(table_lock);
        file = FindFile(table, descriptor);
        if (file == nullptr || !file->writable)
        {
            return User::SyscallErrorBadDescriptor;
        }
        append = file->append;
        ++file->references;
    }
    uint64_t result = 0;
    {
        const VolumeHold hold(DriverOf(file));
        // Under the lock, so writes through other descriptors cannot land in between.
        if (append)
        {
            VFS::Seek(&file->handle, file->handle.size);
        }
        uint64_t bytes_written;
        const bool complete = VFS::Write(&file->handle, buffer, length, &bytes_written);
        // A short write (such as a full disk) still reports the bytes that did land.
        result = complete || bytes_written > 0 ? bytes_written : VFS::LastError();
    }
    Unpin(file);
    return result;
}

uint64_t CreatePipe(Table* table, uint64_t flags, uint64_t* read_end, uint64_t* write_end)
{
    if ((flags & ~(User::PipeCloseOnExec | User::PipeNonBlocking)) != 0)
    {
        return User::SyscallErrorInvalidArgument;
    }
    Sync::MutexGuard guard(table_lock);
    const uint64_t reader = FreeSlotLocked(table, 0);
    const uint64_t writer =
        User::IsSyscallError(reader) ? reader : FreeSlotLocked(table, reader + 1);
    if (User::IsSyscallError(writer))
    {
        return writer;
    }
    OpenFile* const reading = NewFile(Kind::Pipe, true, false);
    OpenFile* const writing = NewFile(Kind::Pipe, false, true);
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
    const bool nonblocking = (flags & User::PipeNonBlocking) != 0;
    reading->pipe = pipe;
    reading->nonblocking = nonblocking;
    writing->pipe = pipe;
    writing->nonblocking = nonblocking;
    const bool close_on_exec = (flags & User::PipeCloseOnExec) != 0;
    table->slots[reader] = {reading, close_on_exec};
    table->slots[writer] = {writing, close_on_exec};
    *read_end = reader;
    *write_end = writer;
    return 0;
}

uint64_t CreateSocket(Table* table, uint64_t family, uint64_t type, uint64_t protocol)
{
    Network::Sockets::Socket* socket = nullptr;
    const uint64_t result = Network::Sockets::Create(family, type, protocol, &socket);
    if (User::IsSyscallError(result))
    {
        return result;
    }
    return AddSocket(table, socket, type & (User::SocketCloseOnExec | User::SocketNonBlocking));
}

uint64_t AddSocket(Table* table, Network::Sockets::Socket* socket, uint64_t flags)
{
    Sync::MutexGuard guard(table_lock);
    const uint64_t descriptor = FreeSlotLocked(table, 0);
    OpenFile* const file =
        User::IsSyscallError(descriptor) ? nullptr : NewFile(Kind::Socket, true, true);
    if (file == nullptr)
    {
        Network::Sockets::Close(socket);
        return User::IsSyscallError(descriptor) ? descriptor : User::SyscallErrorNoMemory;
    }
    file->nonblocking = (flags & User::SocketNonBlocking) != 0;
    file->socket = socket;
    table->slots[descriptor] = {file, (flags & User::SocketCloseOnExec) != 0};
    return descriptor;
}

int16_t PollEvents(Table* table, uint64_t descriptor)
{
    Sync::MutexGuard guard(table_lock);
    const Slot* slot = FindSlot(table, descriptor);
    if (slot == nullptr)
    {
        return User::PollInvalid;
    }
    const OpenFile* file = slot->file;
    switch (file->kind)
    {
    case Kind::Pipe:
        return Pipes::PollEvents(file->pipe, file->writable);
    case Kind::Socket:
        return Network::Sockets::PollEvents(file->socket);
    case Kind::PseudoTerminalMaster:
        return PseudoTerminals::PollMaster(file->pair);
    case Kind::Framebuffer:
        return 0;
    case Kind::PointerEvents:
        return static_cast<int16_t>(Input::Pointer::PollEvents(file->pointer) |
                                    (file->writable ? User::PollWritable : 0));
    default:
        // Files never make a reader or writer wait.
        return static_cast<int16_t>((file->readable ? User::PollReadable : 0) |
                                    (file->writable ? User::PollWritable : 0));
    }
}

void SetFramebufferRelease(FramebufferRelease release)
{
    Sync::MutexGuard guard(table_lock);
    framebuffer_release = release;
}

uint64_t HoldFramebuffer(Table* table, uint64_t descriptor, const void** owner, bool* nonblocking)
{
    Sync::MutexGuard guard(table_lock);
    const Slot* slot = FindSlot(table, descriptor);
    if (slot == nullptr)
    {
        return User::SyscallErrorBadDescriptor;
    }
    if (slot->file->kind != Kind::Framebuffer)
    {
        return User::SyscallErrorNotTerminal;
    }
    ++slot->file->references;
    *owner = slot->file;
    if (nonblocking != nullptr)
    {
        *nonblocking = slot->file->nonblocking;
    }
    return 0;
}

void DropFramebuffer(const void* owner)
{
    Unpin(const_cast<OpenFile*>(static_cast<const OpenFile*>(owner)));
}

uint64_t FindTerminal(Table* table, uint64_t descriptor, Terminals::Terminal** terminal)
{
    Sync::MutexGuard guard(table_lock);
    const Slot* slot = FindSlot(table, descriptor);
    if (slot == nullptr)
    {
        return User::SyscallErrorBadDescriptor;
    }
    if (slot->file->kind != Kind::Terminal || slot->file->terminal == nullptr)
    {
        return User::SyscallErrorNotTerminal;
    }
    *terminal = slot->file->terminal;
    return 0;
}

uint64_t FindPseudoTerminal(Table* table, uint64_t descriptor, PseudoTerminals::Pair** pair)
{
    Sync::MutexGuard guard(table_lock);
    const Slot* slot = FindSlot(table, descriptor);
    if (slot == nullptr)
    {
        return User::SyscallErrorBadDescriptor;
    }
    if (slot->file->kind != Kind::PseudoTerminalMaster)
    {
        return User::SyscallErrorNotTerminal;
    }
    *pair = slot->file->pair;
    return 0;
}

uint64_t FindSocket(Table* table, uint64_t descriptor, Network::Sockets::Socket** socket,
                    bool* nonblocking)
{
    Sync::MutexGuard guard(table_lock);
    const Slot* slot = FindSlot(table, descriptor);
    if (slot == nullptr)
    {
        return User::SyscallErrorBadDescriptor;
    }
    if (slot->file->kind != Kind::Socket)
    {
        return User::SyscallErrorNotSocket;
    }
    *socket = slot->file->socket;
    if (nonblocking != nullptr)
    {
        *nonblocking = slot->file->nonblocking;
    }
    return 0;
}

namespace
{

uint64_t SeekHeld(OpenFile* file, uint64_t offset, uint64_t whence) REQUIRES(held_volume)
{
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

} // namespace

uint64_t Seek(Table* table, uint64_t descriptor, uint64_t offset, uint64_t whence)
{
    OpenFile* file = nullptr;
    {
        Sync::MutexGuard guard(table_lock);
        const Slot* slot = FindSlot(table, descriptor);
        if (slot == nullptr)
        {
            return User::SyscallErrorBadDescriptor;
        }
        if (slot->file->kind != Kind::File)
        {
            return User::SyscallErrorNotSeekable;
        }
        file = slot->file;
        ++file->references;
    }
    uint64_t result = 0;
    {
        const VolumeHold hold(DriverOf(file));
        result = SeekHeld(file, offset, whence);
    }
    Unpin(file);
    return result;
}

uint64_t Close(Table* table, uint64_t descriptor)
{
    OpenFile* closing = nullptr;
    {
        Sync::MutexGuard guard(table_lock);
        Slot* slot = FindSlot(table, descriptor);
        if (slot == nullptr)
        {
            return User::SyscallErrorBadDescriptor;
        }
        closing = DetachSlotLocked(slot);
    }
    FinishClose(closing);
    return 0;
}

namespace
{

uint64_t DuplicateLocked(Table* table, uint64_t descriptor, uint64_t target, uint64_t flags,
                         OpenFile** closing) REQUIRES(table_lock)
{
    const Slot* source = FindSlot(table, descriptor);
    if (source == nullptr)
    {
        return User::SyscallErrorBadDescriptor;
    }
    OpenFile* const file = source->file;
    const bool exact = (flags & User::DuplicateExact) != 0;
    if (target >= table->soft_limit)
    {
        return exact ? User::SyscallErrorBadDescriptor : User::SyscallErrorInvalidArgument;
    }
    uint64_t result = target;
    if (exact)
    {
        // As in dup2, the same descriptor stays as it is.
        if (target == descriptor)
        {
            return target;
        }
        const uint64_t grown = GrowLocked(table, target + 1);
        if (grown != 0)
        {
            return grown;
        }
        if (table->slots[target].file != nullptr)
        {
            *closing = DetachSlotLocked(&table->slots[target]);
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
    ++file->references;
    table->slots[result] = {file, (flags & User::DuplicateCloseOnExec) != 0};
    return result;
}

} // namespace

uint64_t Duplicate(Table* table, uint64_t descriptor, uint64_t target, uint64_t flags)
{
    if ((flags & ~(User::DuplicateExact | User::DuplicateCloseOnExec)) != 0)
    {
        return User::SyscallErrorInvalidArgument;
    }
    OpenFile* closing = nullptr;
    uint64_t result = 0;
    {
        Sync::MutexGuard guard(table_lock);
        result = DuplicateLocked(table, descriptor, target, flags, &closing);
    }
    FinishClose(closing);
    return result;
}

uint64_t DescriptorFlags(Table* table, uint64_t descriptor, bool set, uint64_t value)
{
    if (set && (value & ~User::DescriptorCloseOnExec) != 0)
    {
        return User::SyscallErrorInvalidArgument;
    }
    Sync::MutexGuard guard(table_lock);
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

uint64_t StatusFlags(Table* table, uint64_t descriptor, bool set, uint64_t value)
{
    constexpr uint64_t Changeable = User::OpenAppend | User::OpenNonBlocking;
    if (set && (value & ~(Changeable | User::OpenRead | User::OpenWrite)) != 0)
    {
        return User::SyscallErrorInvalidArgument;
    }
    Sync::MutexGuard guard(table_lock);
    Slot* slot = FindSlot(table, descriptor);
    if (slot == nullptr)
    {
        return User::SyscallErrorBadDescriptor;
    }
    OpenFile* file = slot->file;
    if (set)
    {
        // Every process shares the console's open file, so one program's flags would change
        // everyone's; and terminal reads always wait.
        if (file->kind == Kind::Terminal)
        {
            if ((value & Changeable) != 0)
            {
                return User::SyscallErrorInvalidArgument;
            }
        }
        else
        {
            file->append = (value & User::OpenAppend) != 0;
            file->nonblocking = (value & User::OpenNonBlocking) != 0;
        }
    }
    return (file->readable ? User::OpenRead : 0) | (file->writable ? User::OpenWrite : 0) |
           (file->append ? User::OpenAppend : 0) | (file->nonblocking ? User::OpenNonBlocking : 0);
}

uint64_t CountOpen(const Table* table)
{
    Sync::MutexGuard guard(table_lock);
    return CountOpenLocked(table);
}

uint64_t CountOpenLocked(const Table* table)
{
    uint64_t count = 0;
    for (uint32_t descriptor = 0; descriptor < table->capacity; ++descriptor)
    {
        count += table->slots[descriptor].file != nullptr ? 1 : 0;
    }
    return count;
}

uint64_t Remove(const char* path)
{
    if (path == nullptr)
    {
        return User::SyscallErrorInvalidArgument;
    }
    Resolved resolved;
    const uint64_t walked = resolved.Walk(path, VFS::Follow::NotLast);
    if (walked != 0)
    {
        return walked;
    }
    path = resolved.Path();
    const VolumeHold hold(path);
    // No handle ever opens a link, so nothing can be using it.
    if (IsLink(path))
    {
        if (VFS::IsReadOnly(path))
        {
            return User::SyscallErrorReadOnly;
        }
        return VFS::Remove(path) ? 0 : VFS::LastError();
    }
    VFS::FileHandle handle;
    if (!VFS::Open(path, &handle))
    {
        return VFS::IsDirectory(path) ? User::SyscallErrorIsDirectory : NotFoundError(path);
    }
    const bool removable = VFS::IsTracked(handle);
    const bool busy = removable && InUse(VFS::KeyOf(handle));
    VFS::Close(&handle);
    if (!removable)
    {
        return User::SyscallErrorReadOnly;
    }
    if (busy)
    {
        return User::SyscallErrorBusy;
    }
    return VFS::Remove(path) ? 0 : VFS::LastError();
}

uint64_t ReadDirectory(const char* path, uint64_t index, User::DirectoryEntry* entry)
{
    if (path == nullptr || entry == nullptr || index > UINT32_MAX)
    {
        return User::SyscallErrorInvalidArgument;
    }
    Resolved resolved;
    const uint64_t walked = resolved.Walk(path, VFS::Follow::All);
    if (walked != 0)
    {
        return walked;
    }
    path = resolved.Path();
    if (!VFS::IsDirectory(path))
    {
        return MissingError(path);
    }
    const Memory::HeapObject<Filesystem::DirectoryEntry> found;
    if (!found)
    {
        return User::SyscallErrorNoMemory;
    }
    if (!VFS::ReadDirectory(path, static_cast<uint32_t>(index), &*found))
    {
        return NotFoundError(path);
    }
    static_assert(sizeof(entry->name) == sizeof(found->name));
    *entry = {};
    for (uint32_t offset = 0; offset < sizeof(entry->name) && found->name[offset] != '\0'; ++offset)
    {
        entry->name[offset] = found->name[offset];
    }
    entry->node = found->node;
    entry->size = found->size;
    entry->type = (found->type & User::ModeTypeMask) >> User::EntryTypeShift;
    return 0;
}

uint64_t CheckDirectory(const char* path)
{
    if (path == nullptr)
    {
        return User::SyscallErrorInvalidArgument;
    }
    Resolved resolved;
    const uint64_t walked = resolved.Walk(path, VFS::Follow::All);
    if (walked != 0)
    {
        return walked;
    }
    const VolumeHold hold(resolved.Path());
    return VFS::IsDirectory(resolved.Path()) ? 0 : MissingError(resolved.Path());
}

uint64_t ChangeDirectory(const char* path, char (&working_directory)[User::MaximumPathLength])
{
    if (path == nullptr)
    {
        return User::SyscallErrorInvalidArgument;
    }
    // An unmount, which takes this lock too, sees either the old working directory or the new one.
    Sync::MutexGuard guard(namespace_lock);
    Resolved resolved;
    const uint64_t walked = resolved.Walk(path, VFS::Follow::All);
    if (walked != 0)
    {
        return walked;
    }
    path = resolved.Path();
    const VolumeHold hold(path);
    if (!VFS::IsDirectory(path))
    {
        return MissingError(path);
    }
    uint64_t index = 0;
    for (; path[index] != '\0' && index + 1 < sizeof(working_directory); ++index)
    {
        working_directory[index] = path[index];
    }
    working_directory[index] = '\0';
    return 0;
}

uint64_t Mount(const char* source, const char* point, const char* type, Filesystem::MountMode mode)
{
    if (source == nullptr || point == nullptr || type == nullptr)
    {
        return User::SyscallErrorInvalidArgument;
    }
    Sync::MutexGuard guard(namespace_lock);
    Resolved resolved;
    const uint64_t walked = resolved.Walk(point, VFS::Follow::All);
    if (walked != 0)
    {
        return walked;
    }
    point = resolved.Path();
    if (!VFS::IsMountPoint(point))
    {
        if (!Exists(point))
        {
            if (!ParentExists(point))
            {
                return User::SyscallErrorNotFound;
            }
        }
        else if (!VFS::IsDirectory(point))
        {
            return User::SyscallErrorNotDirectory;
        }
    }
    return MountError(VFS::Mount(type, source, point, mode));
}

uint64_t Unmount(const char* point, WorkingDirectoryCheck in_use)
{
    if (point == nullptr || in_use == nullptr)
    {
        return User::SyscallErrorInvalidArgument;
    }
    Sync::MutexGuard guard(namespace_lock);
    Resolved resolved;
    const uint64_t walked = resolved.Walk(point, VFS::Follow::All);
    if (walked != 0)
    {
        return walked;
    }
    point = resolved.Path();
    if (!VFS::IsMountPoint(point))
    {
        return Exists(point) ? User::SyscallErrorInvalidArgument : User::SyscallErrorNotFound;
    }
    if (in_use(point))
    {
        return User::SyscallErrorBusy;
    }
    const VFS::MountResult result = VFS::Unmount(point);
    return result == VFS::MountResult::NotFound ? User::SyscallErrorInvalidArgument
                                                : MountError(result);
}

uint64_t Remount(const char* point, Filesystem::MountMode mode)
{
    if (point == nullptr)
    {
        return User::SyscallErrorInvalidArgument;
    }
    Sync::MutexGuard guard(namespace_lock);
    Resolved resolved;
    const uint64_t walked = resolved.Walk(point, VFS::Follow::All);
    if (walked != 0)
    {
        return walked;
    }
    point = resolved.Path();
    if (!VFS::IsMountPoint(point))
    {
        return Exists(point) ? User::SyscallErrorInvalidArgument : User::SyscallErrorNotFound;
    }
    // The count of open files and the change are one step: nothing opens a file in between.
    const VolumeHold hold(point);
    const Filesystem::Driver* mounted = VFS::MountedAt(point);
    VFS::OpenFiles open = {};
    {
        Sync::MutexGuard table_guard(table_lock);
        for (uint32_t index = 0; index < disk_file_count; ++index)
        {
            if (disk_files[index].key.filesystem == mounted)
            {
                open.any = true;
                open.writer = open.writer || disk_files[index].writer;
            }
        }
    }
    const VFS::MountResult result = VFS::Remount(point, mode, open);
    return result == VFS::MountResult::NotFound ? User::SyscallErrorInvalidArgument
                                                : MountError(result);
}

uint64_t MakeDirectory(const char* path)
{
    if (path == nullptr)
    {
        return User::SyscallErrorInvalidArgument;
    }
    Resolved resolved;
    const uint64_t walked = resolved.Walk(path, VFS::Follow::NotLast);
    if (walked != 0)
    {
        return walked;
    }
    path = resolved.Path();
    const VolumeHold hold(path);
    if (Exists(path))
    {
        return User::SyscallErrorExists;
    }
    return VFS::MakeDirectory(path) ? 0 : CreateError(path);
}

uint64_t RemoveDirectory(const char* path)
{
    if (path == nullptr)
    {
        return User::SyscallErrorInvalidArgument;
    }
    Resolved resolved;
    const uint64_t walked = resolved.Walk(path, VFS::Follow::NotLast);
    if (walked != 0)
    {
        return walked;
    }
    const LastName last = LastNameOf(path);
    path = resolved.Path();
    const VolumeHold hold(path);
    if (!VFS::IsDirectory(path))
    {
        return MissingError(path);
    }
    // Linux's answers, before any other check.
    if (last != LastName::Plain)
    {
        return last == LastName::Dot ? User::SyscallErrorInvalidArgument
                                     : User::SyscallErrorNotEmpty;
    }
    if (VFS::IsReadOnly(path))
    {
        return User::SyscallErrorReadOnly;
    }
    if (VFS::IsMountPoint(path))
    {
        return User::SyscallErrorBusy;
    }
    if (!VFS::IsEmptyDirectory(path))
    {
        return User::SyscallErrorNotEmpty;
    }
    return VFS::RemoveDirectory(path) ? 0 : VFS::LastError();
}

uint64_t Rename(const char* from, const char* to)
{
    if (from == nullptr || to == nullptr)
    {
        return User::SyscallErrorInvalidArgument;
    }
    Resolved resolved_from;
    Resolved resolved_to;
    uint64_t walked = resolved_from.Walk(from, VFS::Follow::NotLast);
    if (walked == 0)
    {
        walked = resolved_to.Walk(to, VFS::Follow::NotLast);
    }
    if (walked != 0)
    {
        return walked;
    }
    const bool dot_name = LastNameOf(from) != LastName::Plain || LastNameOf(to) != LastName::Plain;
    from = resolved_from.Path();
    to = resolved_to.Path();
    // Two volumes end in an error before any lock is taken; the checks before it run lock by lock.
    const bool same = VFS::SameFilesystem(from, to);
    const VolumeHold hold(same ? from : nullptr);
    if (!Exists(from))
    {
        return User::SyscallErrorNotFound;
    }
    // As Linux: "." and ".." name a directory in use, never an entry to move.
    if (dot_name)
    {
        return User::SyscallErrorBusy;
    }
    if (VFS::IsReadOnly(from) || VFS::IsReadOnly(to))
    {
        return User::SyscallErrorReadOnly;
    }
    if (VFS::IsMountPoint(from) || VFS::IsMountPoint(to))
    {
        return User::SyscallErrorBusy;
    }
    if (!same)
    {
        return User::SyscallErrorCrossDevice;
    }
    if (IsWithin(to, from))
    {
        return IsWithin(from, to) ? 0 : User::SyscallErrorInvalidArgument;
    }
    const bool directory = VFS::IsDirectory(from);
    VFS::FileKey source_key = {};
    if (!directory && !IsLink(from))
    {
        if (!FileKeyOf(from, &source_key))
        {
            return User::SyscallErrorReadOnly;
        }
        // A disk file's handles record where its directory entry is, which a rename moves.
        if (InUse(source_key))
        {
            return User::SyscallErrorBusy;
        }
    }
    VFS::FileInfo target = {};
    if (VFS::Stat(to, &target))
    {
        const uint32_t target_type = target.mode & User::ModeTypeMask;
        if (directory)
        {
            VFS::FileInfo source = {};
            if (target_type != User::ModeDirectory)
            {
                return User::SyscallErrorNotDirectory;
            }
            if (!VFS::Stat(from, &source))
            {
                return User::SyscallErrorNotFound;
            }
            if (target.node != source.node && !VFS::IsEmptyDirectory(to))
            {
                return User::SyscallErrorNotEmpty;
            }
        }
        else if (target_type == User::ModeDirectory)
        {
            return User::SyscallErrorIsDirectory;
        }
        else if (target_type != User::ModeSymbolicLink)
        {
            VFS::FileKey target_key = {};
            if (FileKeyOf(to, &target_key) && target_key != source_key && InUse(target_key))
            {
                return User::SyscallErrorBusy;
            }
        }
    }
    if (VFS::Rename(from, to))
    {
        return 0;
    }
    return ParentExists(to) ? VFS::LastError() : User::SyscallErrorNotFound;
}

uint64_t Stat(const char* path, User::FileStatus* status)
{
    return StatPath(path, status, VFS::Follow::All);
}

uint64_t LinkStat(const char* path, User::FileStatus* status)
{
    return StatPath(path, status, VFS::Follow::NotLast);
}

uint64_t ReadLink(const char* path, char* buffer, uint64_t capacity)
{
    if (path == nullptr || buffer == nullptr || capacity == 0)
    {
        return User::SyscallErrorInvalidArgument;
    }
    Resolved resolved;
    const uint64_t walked = resolved.Walk(path, VFS::Follow::NotLast);
    if (walked != 0)
    {
        return walked;
    }
    path = resolved.Path();
    const VolumeHold hold(path);
    if (!IsLink(path))
    {
        return Exists(path) ? User::SyscallErrorInvalidArgument : User::SyscallErrorNotFound;
    }
    const auto limit = static_cast<uint32_t>(capacity < UINT32_MAX ? capacity : UINT32_MAX);
    uint32_t length = 0;
    if (!VFS::ReadLink(path, buffer, limit, &length))
    {
        return User::SyscallErrorInputOutput;
    }
    return length < limit ? length : limit;
}

uint64_t SymbolicLink(const char* target, const char* path)
{
    if (target == nullptr || path == nullptr)
    {
        return User::SyscallErrorInvalidArgument;
    }
    if (target[0] == '\0')
    {
        return User::SyscallErrorNotFound;
    }
    Resolved resolved;
    const uint64_t walked = resolved.Walk(path, VFS::Follow::NotLast);
    if (walked != 0)
    {
        return walked;
    }
    path = resolved.Path();
    const VolumeHold hold(path);
    const uint64_t refused = NewNameError(path, Filesystem::LinkKind::Symbolic);
    if (refused != 0)
    {
        return refused;
    }
    return VFS::SymbolicLink(path, target) ? 0 : VFS::LastError();
}

uint64_t Link(const char* from, const char* to)
{
    if (from == nullptr || to == nullptr)
    {
        return User::SyscallErrorInvalidArgument;
    }
    Resolved resolved_from;
    Resolved resolved_to;
    uint64_t walked = resolved_from.Walk(from, VFS::Follow::NotLast);
    if (walked == 0)
    {
        walked = resolved_to.Walk(to, VFS::Follow::NotLast);
    }
    if (walked != 0)
    {
        return walked;
    }
    from = resolved_from.Path();
    to = resolved_to.Path();
    const bool same = VFS::SameFilesystem(from, to);
    const VolumeHold hold(same ? from : nullptr);
    if (!Exists(from))
    {
        return User::SyscallErrorNotFound;
    }
    if (Exists(to))
    {
        return User::SyscallErrorExists;
    }
    if (!ParentExists(to))
    {
        return User::SyscallErrorNotFound;
    }
    if (!same)
    {
        return User::SyscallErrorCrossDevice;
    }
    if (VFS::IsDirectory(from))
    {
        return User::SyscallErrorNotPermitted;
    }
    const uint64_t refused = NewNameError(to, Filesystem::LinkKind::Hard);
    if (refused != 0)
    {
        return refused;
    }
    return VFS::Link(from, to) ? 0 : VFS::LastError();
}

uint64_t ResolvePath(const char* path, char* buffer, uint64_t capacity)
{
    if (path == nullptr || buffer == nullptr)
    {
        return User::SyscallErrorInvalidArgument;
    }
    Resolved resolved;
    const uint64_t walked = resolved.Walk(path, VFS::Follow::All);
    if (walked != 0)
    {
        return walked;
    }
    if (!Exists(resolved.Path()))
    {
        return User::SyscallErrorNotFound;
    }
    uint64_t length = 0;
    while (resolved.Path()[length] != '\0')
    {
        ++length;
    }
    if (length >= capacity)
    {
        return User::SyscallErrorRange;
    }
    for (uint64_t index = 0; index <= length; ++index)
    {
        buffer[index] = resolved.Path()[index];
    }
    return length;
}

uint64_t StatDescriptor(Table* table, uint64_t descriptor, User::FileStatus* status)
{
    if (status == nullptr)
    {
        return User::SyscallErrorInvalidArgument;
    }
    OpenFile* file = nullptr;
    {
        Sync::MutexGuard guard(table_lock);
        const Slot* slot = FindSlot(table, descriptor);
        if (slot == nullptr)
        {
            return User::SyscallErrorBadDescriptor;
        }
        file = slot->file;
        switch (file->kind)
        {
        case Kind::Terminal:
            *status = UnmountedStatus(
                file->terminal != nullptr ? static_cast<const void*>(file->terminal) : file,
                User::ModeCharacterDevice | TerminalMode, User::FileType::Terminal, 0, false);
            return 0;
        case Kind::Pipe:
            *status = UnmountedStatus(file->pipe, User::ModeFifo | PipeMode, User::FileType::Pipe,
                                      Pipes::Buffered(file->pipe), !file->writable);
            return 0;
        case Kind::Socket:
            *status = UnmountedStatus(file->socket, User::ModeSocket | SocketMode,
                                      User::FileType::Socket, 0, false);
            return 0;
        case Kind::PseudoTerminalMaster:
            *status = UnmountedStatus(file->pair, User::ModeCharacterDevice | TerminalMode,
                                      User::FileType::Device, 0, false);
            return 0;
        case Kind::Framebuffer:
            *status = UnmountedStatus(file, User::ModeCharacterDevice | FramebufferMode,
                                      User::FileType::Device, 0, false);
            return 0;
        case Kind::PointerEvents:
            *status = UnmountedStatus(file, User::ModeCharacterDevice | PointerMode,
                                      User::FileType::Device, 0, false);
            return 0;
        case Kind::File:
        case Kind::None:
            break;
        }
        ++file->references;
    }
    uint64_t result = 0;
    {
        const VolumeHold hold(DriverOf(file));
        VFS::FileInfo info;
        if (VFS::StatOpen(file->handle, &info))
        {
            *status = ToFileStatus(info, !VFS::IsWritable(file->handle));
        }
        else
        {
            result = User::SyscallErrorFailed;
        }
    }
    Unpin(file);
    return result;
}

uint64_t SyncAll()
{
    return VFS::SyncAll() ? 0 : User::SyscallErrorInputOutput;
}

uint64_t SyncPeriodic()
{
    return VFS::SyncPeriodic() ? 0 : User::SyscallErrorInputOutput;
}

// Held until the machine stops, so nothing writes to the disk after it.
bool Quiesce() NO_THREAD_SAFETY_ANALYSIS
{
    namespace_lock.Lock();
    const bool detached = VFS::DetachAll(true);
    table_lock.Lock();
    return detached;
}

void CloseAll(Table* table)
{
    if (table == nullptr)
    {
        return;
    }
    for (uint32_t descriptor = 0;; ++descriptor)
    {
        OpenFile* closing = nullptr;
        {
            Sync::MutexGuard guard(table_lock);
            if (descriptor >= table->capacity)
            {
                break;
            }
            if (table->slots[descriptor].file != nullptr)
            {
                closing = DetachSlotLocked(&table->slots[descriptor]);
            }
        }
        FinishClose(closing);
    }
    Sync::MutexGuard guard(table_lock);
    if (table->slots != nullptr)
    {
        Memory::Deallocate(table->slots);
    }
    table->slots = nullptr;
    table->capacity = 0;
}

#ifdef FEATHEROS_HOST_TEST
bool ResetDiskFileTable()
{
    Sync::MutexGuard guard(table_lock);
    if (disk_file_count != 0)
    {
        return false;
    }
    if (disk_files != nullptr)
    {
        Memory::Deallocate(disk_files);
    }
    disk_files = nullptr;
    disk_file_capacity = 0;
    return true;
}

namespace Testing
{

void* HoldVolume(const char* path)
{
    return new VolumeHold(path);
}

void ReleaseVolume(void* hold)
{
    delete static_cast<VolumeHold*>(hold);
}

void LockTable()
{
    table_lock.Lock();
}

void UnlockTable()
{
    table_lock.Unlock();
}

void SetPauseHook(PauseHook hook)
{
    pause_hook = hook;
}

} // namespace Testing
#endif

ProgramFile::~ProgramFile()
{
    if (!open)
    {
        return;
    }
    const VolumeHold hold(handle.driver);
    if (tracked)
    {
        Sync::MutexGuard guard(table_lock);
        DropUse(VFS::KeyOf(handle), false);
    }
    VFS::Close(&handle);
}

uint64_t ProgramFile::Open(const char* path)
{
    if (path == nullptr || open)
    {
        return User::SyscallErrorInvalidArgument;
    }
    Resolved resolved;
    const uint64_t walked = resolved.Walk(path, VFS::Follow::All);
    if (walked != 0)
    {
        return walked;
    }
    path = resolved.Path();
    const VolumeHold hold(path);
    if (!VFS::Open(path, &handle))
    {
        return VFS::IsDirectory(path) ? User::SyscallErrorIsDirectory : NotFoundError(path);
    }
    if (VFS::IsDevice(handle) || handle.size == 0)
    {
        VFS::Close(&handle);
        return User::SyscallErrorNotExecutable;
    }
    if (VFS::IsTracked(handle))
    {
        uint64_t added = 0;
        {
            Sync::MutexGuard guard(table_lock);
            added = AddUse(VFS::KeyOf(handle), false);
        }
        if (User::IsSyscallError(added))
        {
            VFS::Close(&handle);
            return added;
        }
        tracked = true;
    }
    open = true;
    return 0;
}

uint64_t ProgramFile::Size() const
{
    return open ? handle.size : 0;
}

bool ProgramFile::ReadAt(uint64_t offset, void* buffer, uint64_t size)
{
    if (!open || offset > handle.size || size > handle.size - offset)
    {
        return false;
    }
    const VolumeHold hold(handle.driver);
    uint64_t bytes_read = 0;
    return VFS::Seek(&handle, offset) && VFS::Read(&handle, buffer, size, &bytes_read) &&
           bytes_read == size;
}

} // namespace FeatherOS::Files
