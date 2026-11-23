#include "core_driver.h"

#include "kernel/klogger.h"

#include "fs/ext2.h"

#include <span>
#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Filesystem
{
namespace
{

using FeatherDisk::Error;
using FeatherDisk::NodeId;
using FeatherDisk::NodeType;
using FeatherDisk::Result;

constexpr uint32_t NewFileMode = 0666;
constexpr uint32_t NewDirectoryMode = 0777;
constexpr uint32_t IncompatibleNeedsRecovery = 0x4;

constinit KLogger logger;

// The core's special nodes (devices, FIFOs, sockets) do not say which kind they are, so they
// report no type.
uint32_t TypeBits(NodeType type)
{
    switch (type)
    {
    case NodeType::File:
        return User::ModeRegular;
    case NodeType::Directory:
        return User::ModeDirectory;
    case NodeType::SymbolicLink:
        return User::ModeSymbolicLink;
    case NodeType::Special:
        break;
    }
    return 0;
}

Timestamp ToTimestamp(const FeatherDisk::Timestamp& time)
{
    return {time.seconds, time.nanoseconds};
}

FileInfo ToFileInfo(const FeatherDisk::Attributes& attributes)
{
    FileInfo info = {};
    info.node = attributes.node;
    info.size = attributes.size;
    info.mode = TypeBits(attributes.type) | (attributes.mode & User::ModePermissionMask);
    info.link_count = attributes.link_count;
    info.owner = attributes.owner;
    info.group = attributes.group;
    info.access_time = ToTimestamp(attributes.access_time);
    info.modify_time = ToTimestamp(attributes.modify_time);
    info.change_time = ToTimestamp(attributes.change_time);
    return info;
}

// The component of path at or after *position, past any '/'s; empty at the end.
std::string_view NextComponent(const char* path, size_t* position)
{
    size_t start = *position;
    while (path[start] == '/')
    {
        ++start;
    }
    size_t end = start;
    while (path[end] != '\0' && path[end] != '/')
    {
        ++end;
    }
    *position = end;
    return {path + start, end - start};
}

} // namespace

uint64_t SyscallError(Error error)
{
    switch (error)
    {
    case Error::NotFound:
        return User::SyscallErrorNotFound;
    case Error::Exists:
        return User::SyscallErrorExists;
    case Error::NotDirectory:
        return User::SyscallErrorNotDirectory;
    case Error::IsDirectory:
        return User::SyscallErrorIsDirectory;
    case Error::NotEmpty:
        return User::SyscallErrorNotEmpty;
    case Error::NoSpace:
        return User::SyscallErrorNoSpace;
    case Error::ReadOnly:
        return User::SyscallErrorReadOnly;
    case Error::InvalidArgument:
        return User::SyscallErrorInvalidArgument;
    case Error::NameTooLong:
        return User::SyscallErrorNameTooLong;
    case Error::FileTooLarge:
        return User::SyscallErrorFileTooLarge;
    case Error::TooManyLinks:
        return User::SyscallErrorTooManyLinks;
    case Error::Busy:
        return User::SyscallErrorBusy;
    case Error::NotPermitted:
        return User::SyscallErrorNotPermitted;
    case Error::OutOfMemory:
        return User::SyscallErrorNoMemory;
    case Error::Unsupported:
        return User::SyscallErrorOperationNotSupported;
    // Linux reports a damaged ext2 volume as EIO too (EUCLEAN is ext4's).
    case Error::Io:
    case Error::Corrupt:
        break;
    }
    return User::SyscallErrorInputOutput;
}

bool CoreDriver::Fail(Error error)
{
    last_error = SyscallError(error);
    return false;
}

void CoreDriver::MarkDead()
{
    dead.store(true, std::memory_order_release);
    ForceReadOnly();
}

bool CoreDriver::Dead() const
{
    return dead.load(std::memory_order_acquire);
}

void CoreDriver::ClearDead()
{
    dead.store(false, std::memory_order_release);
}

void CoreDriver::SetIdentity(const FeatherDisk::BlockDevice* disk,
                             const FeatherDisk::Partition::Range& partition, const char* type_name)
{
    Sync::SpinLockGuard guard(identity_lock);
    identity = {disk, partition, type_name};
}

uint64_t CoreDriver::LastError()
{
    return last_error;
}

bool CoreDriver::CoreReadOnly(MountMode mode) const
{
    return Attributes().read_only || mode == MountMode::ReadOnly;
}

bool CoreDriver::OpenPartition(std::span<const CoreKind> kinds, FeatherDisk::BlockDevice& disk,
                               const FeatherDisk::Partition::Range& partition,
                               FeatherDisk::PageCache* cache)
{
    if (!Forget())
    {
        return false;
    }
    volume.Assign(disk, partition);
    cached.Attach(cache, volume);
    for (const CoreKind& kind : kinds)
    {
        FeatherDisk::FilesystemType& candidate = kind.type();
        if (!candidate.Probe(cached))
        {
            continue;
        }
        const Result<FeatherDisk::Filesystem*> mounted =
            candidate.Mount(cached, {.read_only = true});
        if (mounted)
        {
            type = &candidate;
            filesystem = *mounted;
            range = partition;
            partition_number = partition.number;
            core_read_only = true;
            options = kind.options;
            name = kind.name;
            last_error = User::SyscallErrorFailed;
            SetIdentity(&disk, partition, kind.name);
            return true;
        }
    }
    cached.Detach();
    return false;
}

bool CoreDriver::Forget()
{
    if (Dead())
    {
        Discard();
        return true;
    }
    if (filesystem != nullptr && !type->Unmount(*filesystem))
    {
        return false;
    }
    cached.Detach();
    filesystem = nullptr;
    type = nullptr;
    name = nullptr;
    range = {};
    partition_number = 0;
    options = {};
    core_read_only = true;
    SetIdentity(nullptr, {}, nullptr);
    return true;
}

void CoreDriver::Discard()
{
    if (filesystem != nullptr)
    {
        type->Discard(*filesystem);
    }
    cached.Detach();
    filesystem = nullptr;
    type = nullptr;
    name = nullptr;
    range = {};
    partition_number = 0;
    options = {};
    core_read_only = true;
    SetIdentity(nullptr, {}, nullptr);
}

bool CoreDriver::Overlaps(const FeatherDisk::BlockDevice& disk,
                          const FeatherDisk::Partition::Range& other) const
{
    Sync::SpinLockGuard guard(identity_lock);
    return identity.disk == &disk &&
           identity.range.first_sector < other.first_sector + other.sector_count &&
           other.first_sector < identity.range.first_sector + identity.range.sector_count;
}

bool CoreDriver::Summarize(const FeatherDisk::BlockDevice& disk, Summary* summary) const
{
    Sync::SpinLockGuard guard(identity_lock);
    if (identity.disk != &disk)
    {
        return false;
    }
    *summary = {identity.range.first_sector, identity.range.sector_count, identity.range.number,
                identity.type_name};
    return true;
}

void CoreDriver::InvalidateCache()
{
    cached.Invalidate();
}

const char* CoreDriver::TypeName() const
{
    return type == nullptr ? nullptr : name;
}

bool CoreDriver::OpenForMount(MountMode mode, bool* writable)
{
    if (Dead() || type == nullptr)
    {
        return Fail(Error::Io);
    }
    const bool read_only = CoreReadOnly(mode);
    if (filesystem == nullptr || read_only != core_read_only)
    {
        if (filesystem != nullptr)
        {
            if (const FeatherDisk::Status unmounted = type->Unmount(*filesystem); !unmounted)
            {
                return Fail(unmounted.error());
            }
            filesystem = nullptr;
        }
        core_read_only = read_only;
        Result<FeatherDisk::Filesystem*> mounted = type->Mount(cached, {.read_only = read_only});
        if (!mounted && !read_only)
        {
            mounted = type->Mount(cached, {.read_only = true});
            core_read_only = true;
        }
        if (!mounted)
        {
            core_read_only = true;
            return Fail(mounted.error());
        }
        filesystem = *mounted;
    }
    *writable = filesystem->GetCapabilities().writable;
    return true;
}

bool CoreDriver::RefusesWrites() const
{
    if (Dead())
    {
        return true;
    }
    if (filesystem == nullptr)
    {
        return type != nullptr;
    }
    return !core_read_only && !filesystem->GetCapabilities().writable;
}

bool CoreDriver::RemountReplays()
{
    if (Dead() || !core_read_only || filesystem == nullptr)
    {
        return false;
    }
    const Result<FeatherDisk::Ext::ExtFeatures> features =
        FeatherDisk::Ext::DescribeFeatures(cached);
    return features && (features->incompatible & IncompatibleNeedsRecovery) != 0;
}

bool CoreDriver::ReopenForRemount(MountMode mode, bool* writable)
{
    if (Dead())
    {
        *writable = false;
        last_error = mode == MountMode::ReadWrite ? User::SyscallErrorReadOnly
                                                  : User::SyscallErrorInputOutput;
        return true;
    }
    if (filesystem == nullptr)
    {
        if (!OpenForMount(mode, writable))
        {
            return false;
        }
        if (mode == MountMode::ReadWrite && !*writable)
        {
            last_error = User::SyscallErrorReadOnly;
        }
        return true;
    }
    const bool refusing = RefusesWrites();
    if (mode == MountMode::ReadOnly)
    {
        *writable = false;
        if (core_read_only || refusing)
        {
            return true;
        }
        if (const FeatherDisk::Status closed = filesystem->MakeReadOnly(); !closed)
        {
            return Fail(closed.error());
        }
        core_read_only = true;
        return true;
    }
    if (refusing)
    {
        last_error = User::SyscallErrorReadOnly;
        *writable = false;
        return true;
    }
    if (!core_read_only)
    {
        *writable = true;
        return true;
    }
    if (!OpenForMount(MountMode::ReadWrite, writable))
    {
        return false;
    }
    if (!*writable)
    {
        last_error = User::SyscallErrorReadOnly;
    }
    return true;
}

size_t CoreDriver::PartitionNumber() const
{
    return partition_number;
}

bool CoreDriver::Statistics(FeatherDisk::VolumeStatistics* statistics)
{
    if (Dead() || filesystem == nullptr || statistics == nullptr)
    {
        return false;
    }
    const Result<FeatherDisk::VolumeStatistics> result = filesystem->Statistics();
    if (!result)
    {
        return false;
    }
    *statistics = *result;
    return true;
}

Result<NodeId> CoreDriver::ResolveParent(const char* path, std::string_view* leaf)
{
    if (Dead())
    {
        Fail(Error::Io);
        return std::unexpected(Error::Io);
    }
    if (filesystem == nullptr)
    {
        return std::unexpected(FeatherDisk::Error::Io);
    }
    if (path == nullptr)
    {
        return std::unexpected(FeatherDisk::Error::InvalidArgument);
    }
    NodeId directory = filesystem->Root();
    size_t position = 0;
    std::string_view component = NextComponent(path, &position);
    for (;;)
    {
        const std::string_view next = NextComponent(path, &position);
        if (next.empty())
        {
            *leaf = component;
            return directory;
        }
        const Result<NodeId> found = filesystem->Lookup(directory, component);
        if (!found)
        {
            return found;
        }
        directory = *found;
        component = next;
    }
}

Result<NodeId> CoreDriver::Resolve(const char* path)
{
    std::string_view leaf;
    const Result<NodeId> parent = ResolveParent(path, &leaf);
    if (!parent || leaf.empty())
    {
        return parent;
    }
    return filesystem->Lookup(*parent, leaf);
}

bool CoreDriver::IsKind(NodeId node, NodeType kind)
{
    const Result<FeatherDisk::Attributes> attributes = filesystem->GetAttributes(node);
    return attributes && attributes->type == kind;
}

bool CoreDriver::Open(const char* path, Node* node, uint64_t* size)
{
    const Result<NodeId> found = Resolve(path);
    if (!found)
    {
        return false;
    }
    const Result<FeatherDisk::Attributes> attributes = filesystem->GetAttributes(*found);
    if (!attributes || attributes->type != NodeType::File)
    {
        return false;
    }
    *node = {*found, 0};
    *size = attributes->size;
    return true;
}

bool CoreDriver::Stat(const char* path, FileInfo* info)
{
    const Result<NodeId> found = Resolve(path);
    if (!found)
    {
        return false;
    }
    return StatNode({*found, 0}, info);
}

bool CoreDriver::StatNode(const Node& node, FileInfo* info)
{
    if (Dead() || filesystem == nullptr)
    {
        return Fail(Error::Io);
    }
    const Result<FeatherDisk::Attributes> attributes = filesystem->GetAttributes(node.id);
    if (!attributes)
    {
        return false;
    }
    *info = ToFileInfo(*attributes);
    return true;
}

bool CoreDriver::Read(const Node& node, uint64_t size, uint64_t offset, void* buffer,
                      uint64_t length, uint64_t* bytes_read)
{
    *bytes_read = 0;
    if (Dead() || filesystem == nullptr)
    {
        return Fail(Error::Io);
    }
    if (offset > size)
    {
        return false;
    }
    const uint64_t count = length < size - offset ? length : size - offset;
    if (count == 0)
    {
        return true;
    }
    const Result<size_t> read = filesystem->Read(
        node.id, offset, std::span(static_cast<uint8_t*>(buffer), static_cast<size_t>(count)));
    if (!read)
    {
        return false;
    }
    *bytes_read = *read;
    return true;
}

bool CoreDriver::Create(const char* path, Node* node, uint64_t* size)
{
    std::string_view leaf;
    const Result<NodeId> parent = ResolveParent(path, &leaf);
    if (!parent)
    {
        return Fail(parent.error());
    }
    if (leaf.empty())
    {
        return Fail(Error::IsDirectory);
    }
    const Result<NodeId> created = filesystem->Create(*parent, leaf, NewFileMode);
    if (!created)
    {
        return Fail(created.error());
    }
    *node = {*created, 0};
    *size = 0;
    return true;
}

bool CoreDriver::Write(Node* node, uint64_t* size, uint64_t offset, const void* buffer,
                       uint64_t length, uint64_t* bytes_written)
{
    *bytes_written = 0;
    if (Dead() || filesystem == nullptr)
    {
        return Fail(Error::Io);
    }
    if (offset > *size || (length != 0 && buffer == nullptr))
    {
        return Fail(Error::InvalidArgument);
    }
    const Result<size_t> written = filesystem->Write(
        node->id, offset,
        std::span(static_cast<const uint8_t*>(buffer), static_cast<size_t>(length)));
    if (!written)
    {
        return Fail(written.error());
    }
    *bytes_written = *written;
    if (offset + *written > *size)
    {
        *size = offset + *written;
    }
    // The core stops short only when the volume is full.
    return *written == length || Fail(Error::NoSpace);
}

bool CoreDriver::Truncate(Node* node, uint64_t* size, uint64_t new_size)
{
    if (Dead() || filesystem == nullptr)
    {
        return Fail(Error::Io);
    }
    if (new_size > *size)
    {
        return Fail(Error::InvalidArgument);
    }
    const FeatherDisk::Status truncated = filesystem->Truncate(node->id, new_size);
    if (truncated)
    {
        *size = new_size;
        return true;
    }
    // A failure part way may have shrunk the file already.
    const Result<FeatherDisk::Attributes> attributes = filesystem->GetAttributes(node->id);
    if (attributes)
    {
        *size = attributes->size;
    }
    return Fail(truncated.error());
}

bool CoreDriver::Remove(const char* path)
{
    std::string_view leaf;
    const Result<NodeId> parent = ResolveParent(path, &leaf);
    if (!parent)
    {
        return Fail(parent.error());
    }
    if (leaf.empty())
    {
        return Fail(Error::IsDirectory);
    }
    const FeatherDisk::Status removed = filesystem->Remove(*parent, leaf);
    return removed || Fail(removed.error());
}

bool CoreDriver::MakeDirectory(const char* path)
{
    std::string_view leaf;
    const Result<NodeId> parent = ResolveParent(path, &leaf);
    if (!parent)
    {
        return Fail(parent.error());
    }
    if (leaf.empty())
    {
        return Fail(Error::Exists);
    }
    const Result<NodeId> made = filesystem->MakeDirectory(*parent, leaf, NewDirectoryMode);
    return made || Fail(made.error());
}

bool CoreDriver::RemoveDirectory(const char* path)
{
    std::string_view leaf;
    const Result<NodeId> parent = ResolveParent(path, &leaf);
    if (!parent)
    {
        return Fail(parent.error());
    }
    if (leaf.empty())
    {
        return Fail(Error::Busy);
    }
    const FeatherDisk::Status removed = filesystem->RemoveDirectory(*parent, leaf);
    return removed || Fail(removed.error());
}

bool CoreDriver::Rename(const char* from, const char* to)
{
    std::string_view from_leaf;
    std::string_view to_leaf;
    const Result<NodeId> from_parent = ResolveParent(from, &from_leaf);
    if (!from_parent)
    {
        return Fail(from_parent.error());
    }
    const Result<NodeId> to_parent = ResolveParent(to, &to_leaf);
    if (!to_parent)
    {
        return Fail(to_parent.error());
    }
    if (from_leaf.empty() || to_leaf.empty())
    {
        return Fail(Error::Busy);
    }
    const Result<NodeId> source = filesystem->Lookup(*from_parent, from_leaf);
    if (!source)
    {
        return Fail(source.error());
    }
    const FeatherDisk::Status renamed =
        filesystem->Rename(*from_parent, from_leaf, *to_parent, to_leaf);
    return renamed || Fail(renamed.error());
}

bool CoreDriver::IsDirectory(const char* path)
{
    const Result<NodeId> found = Resolve(path);
    return found && IsKind(*found, NodeType::Directory);
}

bool CoreDriver::ReadDirectory(const char* path, uint32_t index, DirectoryEntry* entry)
{
    const Result<NodeId> directory = Resolve(path);
    if (!directory)
    {
        return false;
    }
    uint64_t cookie = 0;
    FeatherDisk::DirectoryEntry found{};
    for (uint32_t position = 0; position <= index; ++position)
    {
        const Result<bool> more = filesystem->ReadDirectory(*directory, cookie, found);
        if (!more || !*more)
        {
            return false;
        }
    }
    static_assert(sizeof(entry->name) > FeatherDisk::MaximumNameLength);
    if (found.name_length > FeatherDisk::MaximumNameLength)
    {
        return false;
    }
    for (size_t offset = 0; offset < found.name_length; ++offset)
    {
        entry->name[offset] = found.name[offset];
    }
    entry->name[found.name_length] = '\0';
    entry->type = TypeBits(found.type);
    entry->node = found.node;
    entry->size = 0;
    if (found.type != NodeType::Directory)
    {
        const Result<FeatherDisk::Attributes> attributes = filesystem->GetAttributes(found.node);
        if (!attributes)
        {
            return false;
        }
        entry->size = attributes->size;
    }
    return true;
}

bool CoreDriver::SupportsLinks(LinkKind kind)
{
    if (Dead() || filesystem == nullptr)
    {
        return false;
    }
    const FeatherDisk::Capabilities capabilities = filesystem->GetCapabilities();
    return kind == LinkKind::Symbolic ? capabilities.symbolic_links : capabilities.hard_links;
}

bool CoreDriver::FindLink(const char* path, uint32_t* link_end)
{
    if (path == nullptr || link_end == nullptr || !SupportsLinks(LinkKind::Symbolic))
    {
        return false;
    }
    NodeId directory = filesystem->Root();
    size_t position = 0;
    for (;;)
    {
        const std::string_view component = NextComponent(path, &position);
        if (component.empty())
        {
            return false;
        }
        const Result<NodeId> found = filesystem->Lookup(directory, component);
        if (!found)
        {
            return false;
        }
        const Result<FeatherDisk::Attributes> attributes = filesystem->GetAttributes(*found);
        if (!attributes)
        {
            return false;
        }
        if (attributes->type == NodeType::SymbolicLink)
        {
            *link_end = static_cast<uint32_t>(position);
            return true;
        }
        if (attributes->type != NodeType::Directory)
        {
            return false;
        }
        directory = *found;
    }
}

bool CoreDriver::ReadLink(const char* path, char* buffer, uint32_t capacity, uint32_t* length)
{
    if (buffer == nullptr || length == nullptr)
    {
        return false;
    }
    const Result<NodeId> found = Resolve(path);
    if (!found)
    {
        return false;
    }
    const Result<FeatherDisk::Attributes> attributes = filesystem->GetAttributes(*found);
    if (!attributes || attributes->type != NodeType::SymbolicLink || attributes->size > UINT32_MAX)
    {
        return false;
    }
    const auto size = static_cast<uint32_t>(attributes->size);
    const uint32_t count = size < capacity ? size : capacity;
    const Result<size_t> read = filesystem->ReadLink(*found, std::span(buffer, count));
    if (!read || *read != count)
    {
        return false;
    }
    *length = size;
    return true;
}

bool CoreDriver::SymbolicLink(const char* path, const char* target)
{
    if (target == nullptr)
    {
        return Fail(Error::InvalidArgument);
    }
    std::string_view leaf;
    const Result<NodeId> parent = ResolveParent(path, &leaf);
    if (!parent)
    {
        return Fail(parent.error());
    }
    if (leaf.empty())
    {
        return Fail(Error::Exists);
    }
    const Result<NodeId> made = filesystem->SymbolicLink(*parent, leaf, target);
    return made || Fail(made.error());
}

bool CoreDriver::Link(const char* from, const char* to)
{
    const Result<NodeId> source = Resolve(from);
    if (!source)
    {
        return Fail(source.error());
    }
    std::string_view leaf;
    const Result<NodeId> parent = ResolveParent(to, &leaf);
    if (!parent)
    {
        return Fail(parent.error());
    }
    if (leaf.empty())
    {
        return Fail(Error::Exists);
    }
    const FeatherDisk::Status linked = filesystem->Link(*source, *parent, leaf);
    return linked || Fail(linked.error());
}

bool CoreDriver::Sync()
{
    if (Dead())
    {
        return Fail(Error::Io);
    }
    if (filesystem == nullptr)
    {
        return true;
    }
    const FeatherDisk::Status synced = filesystem->Sync();
    return synced || Fail(synced.error());
}

bool CoreDriver::Detach()
{
    if (Dead())
    {
        Discard();
        return true;
    }
    const bool done = DetachCore();
    cached.Invalidate();
    return done;
}

bool CoreDriver::DetachCore()
{
    if (!options.close_on_detach || filesystem == nullptr || core_read_only)
    {
        return Sync();
    }
    if (const FeatherDisk::Status closed = type->Unmount(*filesystem); !closed)
    {
        return Fail(closed.error());
    }
    filesystem = nullptr;
    core_read_only = true;
    const Result<FeatherDisk::Filesystem*> reopened = type->Mount(cached, {.read_only = true});
    if (!reopened)
    {
        logger.Error("ext: filesystem could not be reopened read-only");
        return true;
    }
    filesystem = *reopened;
    return true;
}

bool CoreDriver::SyncsInBackground() const
{
    return !Dead() && options.periodic_sync && filesystem != nullptr && !core_read_only &&
           filesystem->GetCapabilities().writable;
}

} // namespace FeatherOS::Filesystem
