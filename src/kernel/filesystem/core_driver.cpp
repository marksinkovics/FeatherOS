#include "core_driver.h"

#include <array>
#include <span>
#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Filesystem
{
namespace
{

using FeatherDisk::NodeId;
using FeatherDisk::NodeType;
using FeatherDisk::Result;

constexpr size_t MaximumPartitions = 16;
constexpr uint32_t NewFileMode = 0666;
constexpr uint32_t NewDirectoryMode = 0777;

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

bool CoreDriver::Mount(FeatherDisk::FilesystemType& new_type, FeatherDisk::BlockDevice& disk)
{
    if (filesystem != nullptr)
    {
        if (!type->Unmount(*filesystem))
        {
            return false;
        }
        filesystem = nullptr;
        type = nullptr;
    }
    std::array<FeatherDisk::Partition::Range, MaximumPartitions> partitions{};
    const size_t count = FeatherDisk::Partition::Find(disk, partitions);
    for (size_t index = 0; index < count; ++index)
    {
        volume.Assign(disk, partitions[index]);
        if (!new_type.Probe(volume))
        {
            continue;
        }
        const Result<FeatherDisk::Filesystem*> mounted =
            new_type.Mount(volume, {.read_only = Attributes().read_only});
        if (mounted)
        {
            type = &new_type;
            filesystem = *mounted;
            partition_number = index + 1;
            return true;
        }
    }
    return false;
}

size_t CoreDriver::PartitionNumber() const
{
    return filesystem == nullptr ? 0 : partition_number;
}

bool CoreDriver::Statistics(FeatherDisk::VolumeStatistics* statistics)
{
    if (filesystem == nullptr || statistics == nullptr)
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
    if (filesystem == nullptr || path == nullptr)
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
    if (filesystem == nullptr)
    {
        return false;
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
    if (filesystem == nullptr || offset > size)
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
    if (!parent || leaf.empty())
    {
        return false;
    }
    const Result<NodeId> created = filesystem->Create(*parent, leaf, NewFileMode);
    if (!created)
    {
        return false;
    }
    *node = {*created, 0};
    *size = 0;
    return true;
}

bool CoreDriver::Write(Node* node, uint64_t* size, uint64_t offset, const void* buffer,
                       uint64_t length, uint64_t* bytes_written)
{
    *bytes_written = 0;
    if (filesystem == nullptr || offset > *size || (length != 0 && buffer == nullptr))
    {
        return false;
    }
    const Result<size_t> written = filesystem->Write(
        node->id, offset,
        std::span(static_cast<const uint8_t*>(buffer), static_cast<size_t>(length)));
    if (!written)
    {
        return false;
    }
    *bytes_written = *written;
    if (offset + *written > *size)
    {
        *size = offset + *written;
    }
    return *written == length;
}

bool CoreDriver::Truncate(Node* node, uint64_t* size, uint64_t new_size)
{
    if (filesystem == nullptr || new_size > *size)
    {
        return false;
    }
    if (filesystem->Truncate(node->id, new_size))
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
    return false;
}

bool CoreDriver::Remove(const char* path)
{
    std::string_view leaf;
    const Result<NodeId> parent = ResolveParent(path, &leaf);
    return parent && !leaf.empty() && filesystem->Remove(*parent, leaf);
}

bool CoreDriver::MakeDirectory(const char* path)
{
    std::string_view leaf;
    const Result<NodeId> parent = ResolveParent(path, &leaf);
    return parent && !leaf.empty() && filesystem->MakeDirectory(*parent, leaf, NewDirectoryMode);
}

bool CoreDriver::RemoveDirectory(const char* path)
{
    std::string_view leaf;
    const Result<NodeId> parent = ResolveParent(path, &leaf);
    return parent && !leaf.empty() && filesystem->RemoveDirectory(*parent, leaf);
}

bool CoreDriver::Rename(const char* from, const char* to)
{
    std::string_view from_leaf;
    std::string_view to_leaf;
    const Result<NodeId> from_parent = ResolveParent(from, &from_leaf);
    const Result<NodeId> to_parent = ResolveParent(to, &to_leaf);
    if (!from_parent || !to_parent || from_leaf.empty() || to_leaf.empty())
    {
        return false;
    }
    const Result<NodeId> source = filesystem->Lookup(*from_parent, from_leaf);
    if (!source)
    {
        return false;
    }
    const Result<NodeId> target = filesystem->Lookup(*to_parent, to_leaf);
    if (target ? *target != *source : target.error() != FeatherDisk::Error::NotFound)
    {
        return false;
    }
    return filesystem->Rename(*from_parent, from_leaf, *to_parent, to_leaf).has_value();
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
    if (filesystem == nullptr)
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
    std::string_view leaf;
    const Result<NodeId> parent = ResolveParent(path, &leaf);
    return target != nullptr && parent && !leaf.empty() &&
           filesystem->SymbolicLink(*parent, leaf, target).has_value();
}

bool CoreDriver::Link(const char* from, const char* to)
{
    std::string_view leaf;
    const Result<NodeId> source = Resolve(from);
    const Result<NodeId> parent = ResolveParent(to, &leaf);
    return source && parent && !leaf.empty() &&
           filesystem->Link(*source, *parent, leaf).has_value();
}

bool CoreDriver::Sync()
{
    return filesystem == nullptr || filesystem->Sync().has_value();
}

} // namespace FeatherOS::Filesystem
