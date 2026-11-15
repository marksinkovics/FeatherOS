#include "core_driver.h"

#include <array>
#include <span>
#include <stddef.h>

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
            return true;
        }
    }
    return false;
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
    Node node = {};
    uint64_t size = 0;
    if (!Open(path, &node, &size))
    {
        return false;
    }
    info->size = size;
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
    // Longer names than the VFS can carry are cut short.
    const size_t length =
        found.name_length < sizeof(entry->name) ? found.name_length : sizeof(entry->name) - 1;
    for (size_t offset = 0; offset < length; ++offset)
    {
        entry->name[offset] = found.name[offset];
    }
    entry->name[length] = '\0';
    entry->directory = found.type == NodeType::Directory;
    entry->size = 0;
    if (!entry->directory)
    {
        const Result<FeatherDisk::Attributes> attributes = filesystem->GetAttributes(found.node);
        if (!attributes)
        {
            return false;
        }
        entry->size = static_cast<uint32_t>(attributes->size);
    }
    return true;
}

bool CoreDriver::Sync()
{
    return filesystem == nullptr || filesystem->Sync().has_value();
}

} // namespace FeatherOS::Filesystem
