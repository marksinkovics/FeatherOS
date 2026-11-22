#include "tmpfs.h"

#include "kernel/memory/heap.h"

namespace FeatherOS::Tmpfs
{
namespace
{

// Each root's node is its index.
constexpr uint32_t RootCount = 2;
constexpr uint32_t MaximumNodes = RootCount + MaximumEntries;
constexpr uint32_t NameCapacity = User::MaximumNameLength + 1;
constexpr uint64_t MinimumCapacity = 256;

struct Node
{
    char name[NameCapacity];
    uint32_t parent;
    bool used;
    bool directory;
    // Its data is the target.
    bool link;
    uint8_t* data;
    uint64_t size;
    uint64_t capacity;
};

Node nodes[MaximumNodes];
// The sum of every file's capacity, which MaximumBytes limits.
uint64_t used_bytes = 0;

uint32_t RootNode(Root root)
{
    return static_cast<uint32_t>(root);
}

bool IsRoot(uint32_t node)
{
    return node < RootCount;
}

void EnsureRoot(Root root)
{
    Node& node = nodes[RootNode(root)];
    if (!node.used)
    {
        node = {};
        node.used = true;
        node.directory = true;
    }
}

bool NameMatches(const Node& node, const char* name, uint32_t length)
{
    for (uint32_t index = 0; index < length; ++index)
    {
        if (node.name[index] != name[index])
        {
            return false;
        }
    }
    return node.name[length] == '\0';
}

bool FindChild(uint32_t parent, const char* name, uint32_t length, uint32_t* child)
{
    for (uint32_t index = RootCount; index < MaximumNodes; ++index)
    {
        if (nodes[index].used && nodes[index].parent == parent &&
            NameMatches(nodes[index], name, length))
        {
            *child = index;
            return true;
        }
    }
    return false;
}

// Walks the first length characters of path.
bool LookupPrefix(Root root, const char* path, uint32_t length, uint32_t* node)
{
    EnsureRoot(root);
    uint32_t current = RootNode(root);
    uint32_t position = 0;
    while (position < length)
    {
        uint32_t component = 0;
        while (position + component < length && path[position + component] != '/')
        {
            ++component;
        }
        if (component == 0 || !nodes[current].directory ||
            !FindChild(current, path + position, component, &current))
        {
            return false;
        }
        position += component;
        if (position < length)
        {
            ++position;
        }
    }
    *node = current;
    return true;
}

bool Lookup(Root root, const char* path, uint32_t* node)
{
    uint32_t length = 0;
    while (path[length] != '\0')
    {
        ++length;
    }
    return LookupPrefix(root, path, length, node);
}

// Finds the directory that will hold path's last component, and where that name starts.
bool LookupParent(Root root, const char* path, uint32_t* parent, const char** name)
{
    uint32_t last_separator = 0;
    bool has_separator = false;
    uint32_t length = 0;
    for (; path[length] != '\0'; ++length)
    {
        if (path[length] == '/')
        {
            last_separator = length;
            has_separator = true;
        }
    }
    *name = has_separator ? path + last_separator + 1 : path;
    const uint32_t name_length = length - static_cast<uint32_t>(*name - path);
    if (name_length == 0 || name_length >= NameCapacity ||
        (name_length == 1 && (*name)[0] == '.') ||
        (name_length == 2 && (*name)[0] == '.' && (*name)[1] == '.'))
    {
        return false;
    }
    if (!has_separator)
    {
        EnsureRoot(root);
        *parent = RootNode(root);
        return true;
    }
    return LookupPrefix(root, path, last_separator, parent) && nodes[*parent].directory;
}

bool AddNode(Root root, const char* path, bool directory, uint32_t* node)
{
    uint32_t parent;
    const char* name;
    uint32_t existing;
    if (!LookupParent(root, path, &parent, &name) || Lookup(root, path, &existing))
    {
        return false;
    }
    for (uint32_t index = RootCount; index < MaximumNodes; ++index)
    {
        if (nodes[index].used)
        {
            continue;
        }
        nodes[index] = {};
        uint32_t length = 0;
        for (; name[length] != '\0'; ++length)
        {
            nodes[index].name[length] = name[length];
        }
        nodes[index].name[length] = '\0';
        nodes[index].parent = parent;
        nodes[index].used = true;
        nodes[index].directory = directory;
        *node = index;
        return true;
    }
    return false;
}

void FreeData(Node& node)
{
    if (node.data != nullptr)
    {
        Memory::Deallocate(node.data);
    }
    used_bytes -= node.capacity;
    node.data = nullptr;
    node.capacity = 0;
    node.size = 0;
}

// Grows the file's buffer to hold at least wanted bytes, doubling when the limit allows. Returns
// the capacity it ended with, which may be less than wanted when memory runs out.
uint64_t Reserve(Node& node, uint64_t wanted)
{
    if (wanted <= node.capacity)
    {
        return node.capacity;
    }
    const uint64_t available = MaximumBytes - used_bytes + node.capacity;
    uint64_t capacity = node.capacity * 2;
    capacity = capacity < MinimumCapacity ? MinimumCapacity : capacity;
    capacity = capacity < wanted ? wanted : capacity;
    capacity = capacity > available ? available : capacity;
    if (capacity <= node.capacity)
    {
        return node.capacity;
    }
    auto* data = static_cast<uint8_t*>(Memory::Allocate(capacity));
    if (data == nullptr)
    {
        return node.capacity;
    }
    for (uint64_t index = 0; index < node.size; ++index)
    {
        data[index] = node.data[index];
    }
    if (node.data != nullptr)
    {
        Memory::Deallocate(node.data);
    }
    used_bytes += capacity - node.capacity;
    node.data = data;
    node.capacity = capacity;
    return capacity;
}

bool IsFile(uint32_t node)
{
    return node < MaximumNodes && nodes[node].used && !nodes[node].directory && !nodes[node].link;
}

bool HasData(uint32_t node)
{
    return node < MaximumNodes && nodes[node].used && !nodes[node].directory;
}

bool HasChild(uint32_t directory)
{
    for (uint32_t child = RootCount; child < MaximumNodes; ++child)
    {
        if (nodes[child].used && nodes[child].parent == directory)
        {
            return true;
        }
    }
    return false;
}

} // namespace

bool Find(Root root, const char* path, uint32_t* node)
{
    return path != nullptr && node != nullptr && Lookup(root, path, node);
}

bool IsDirectoryNode(uint32_t node)
{
    return node < MaximumNodes && nodes[node].used && nodes[node].directory;
}

bool IsLinkNode(uint32_t node)
{
    return node < MaximumNodes && nodes[node].used && nodes[node].link;
}

uint64_t SerialNumber(uint32_t node)
{
    return uint64_t{node} + 1;
}

bool Open(Root root, const char* path, uint32_t* node)
{
    uint32_t found;
    if (path == nullptr || node == nullptr || !Lookup(root, path, &found) || !IsFile(found))
    {
        return false;
    }
    *node = found;
    return true;
}

bool Create(Root root, const char* path, uint32_t* node)
{
    return path != nullptr && node != nullptr && AddNode(root, path, false, node);
}

uint64_t Size(uint32_t node)
{
    return HasData(node) ? nodes[node].size : 0;
}

bool Read(uint32_t node, uint64_t offset, void* buffer, uint64_t length, uint64_t* bytes_read)
{
    *bytes_read = 0;
    if (!IsFile(node) || offset > nodes[node].size || (length > 0 && buffer == nullptr))
    {
        return false;
    }
    const uint64_t available = nodes[node].size - offset;
    const uint64_t count = length < available ? length : available;
    auto* destination = static_cast<uint8_t*>(buffer);
    for (uint64_t index = 0; index < count; ++index)
    {
        destination[index] = nodes[node].data[offset + index];
    }
    *bytes_read = count;
    return true;
}

bool Write(uint32_t node, uint64_t offset, const void* buffer, uint64_t length,
           uint64_t* bytes_written)
{
    *bytes_written = 0;
    if (!IsFile(node) || offset > nodes[node].size || (length > 0 && buffer == nullptr) ||
        length > MaximumBytes)
    {
        return false;
    }
    Node& file = nodes[node];
    const uint64_t capacity = Reserve(file, offset + length);
    const uint64_t room = capacity - offset;
    const uint64_t count = length < room ? length : room;
    const auto* source = static_cast<const uint8_t*>(buffer);
    for (uint64_t index = 0; index < count; ++index)
    {
        file.data[offset + index] = source[index];
    }
    if (offset + count > file.size)
    {
        file.size = offset + count;
    }
    *bytes_written = count;
    return count == length;
}

bool Truncate(uint32_t node, uint64_t size)
{
    if (!IsFile(node) || size > nodes[node].size)
    {
        return false;
    }
    if (size == 0)
    {
        FreeData(nodes[node]);
        return true;
    }
    nodes[node].size = size;
    return true;
}

bool Remove(Root root, const char* path)
{
    uint32_t node;
    if (path == nullptr || !Lookup(root, path, &node) || !HasData(node))
    {
        return false;
    }
    FreeData(nodes[node]);
    nodes[node].used = false;
    return true;
}

bool MakeDirectory(Root root, const char* path)
{
    uint32_t node;
    return path != nullptr && AddNode(root, path, true, &node);
}

bool RemoveDirectory(Root root, const char* path)
{
    uint32_t node;
    if (path == nullptr || !Lookup(root, path, &node) || IsRoot(node) || !nodes[node].directory ||
        HasChild(node))
    {
        return false;
    }
    nodes[node].used = false;
    return true;
}

bool Rename(Root root, const char* from, const char* to)
{
    uint32_t node;
    uint32_t parent;
    const char* name;
    uint32_t existing;
    if (from == nullptr || to == nullptr || !Lookup(root, from, &node) || IsRoot(node) ||
        !LookupParent(root, to, &parent, &name))
    {
        return false;
    }
    for (uint32_t ancestor = parent; !IsRoot(ancestor); ancestor = nodes[ancestor].parent)
    {
        if (ancestor == node)
        {
            return false;
        }
    }
    const bool replaces = Lookup(root, to, &existing);
    if (replaces)
    {
        if (existing == node)
        {
            return true;
        }
        if (IsRoot(existing) || nodes[existing].directory != nodes[node].directory ||
            (nodes[existing].directory && HasChild(existing)))
        {
            return false;
        }
        FreeData(nodes[existing]);
        nodes[existing].used = false;
    }
    uint32_t length = 0;
    for (; name[length] != '\0'; ++length)
    {
        nodes[node].name[length] = name[length];
    }
    nodes[node].name[length] = '\0';
    nodes[node].parent = parent;
    return true;
}

bool IsDirectory(Root root, const char* path)
{
    uint32_t node;
    return path != nullptr && Lookup(root, path, &node) && nodes[node].directory;
}

bool ReadDirectory(Root root, const char* path, uint32_t index, Filesystem::DirectoryEntry* entry)
{
    uint32_t directory;
    if (path == nullptr || entry == nullptr || !Lookup(root, path, &directory) ||
        !nodes[directory].directory)
    {
        return false;
    }
    uint32_t seen = 0;
    for (uint32_t node = RootCount; node < MaximumNodes; ++node)
    {
        if (!nodes[node].used || nodes[node].parent != directory || seen++ != index)
        {
            continue;
        }
        for (uint32_t offset = 0; offset < NameCapacity; ++offset)
        {
            entry->name[offset] = nodes[node].name[offset];
        }
        entry->type = nodes[node].directory ? User::ModeDirectory
                      : nodes[node].link    ? User::ModeSymbolicLink
                                            : User::ModeRegular;
        entry->node = SerialNumber(node);
        entry->size = nodes[node].size;
        return true;
    }
    return false;
}

bool SymbolicLink(Root root, const char* path, const char* target, uint32_t* node)
{
    if (path == nullptr || target == nullptr || node == nullptr)
    {
        return false;
    }
    uint64_t length = 0;
    while (target[length] != '\0')
    {
        if (++length >= User::MaximumPathLength)
        {
            return false;
        }
    }
    uint32_t created;
    if (!AddNode(root, path, false, &created))
    {
        return false;
    }
    Node& link = nodes[created];
    link.link = true;
    if (Reserve(link, length) < length)
    {
        FreeData(link);
        link.used = false;
        return false;
    }
    for (uint64_t index = 0; index < length; ++index)
    {
        link.data[index] = static_cast<uint8_t>(target[index]);
    }
    link.size = length;
    *node = created;
    return true;
}

bool ReadLink(uint32_t node, char* buffer, uint32_t capacity, uint32_t* length)
{
    if (!IsLinkNode(node) || buffer == nullptr || length == nullptr)
    {
        return false;
    }
    const uint64_t size = nodes[node].size;
    const uint64_t count = size < capacity ? size : capacity;
    for (uint64_t index = 0; index < count; ++index)
    {
        buffer[index] = static_cast<char>(nodes[node].data[index]);
    }
    *length = static_cast<uint32_t>(size);
    return true;
}

bool FindLink(Root root, const char* path, uint32_t* link_end)
{
    if (path == nullptr || link_end == nullptr)
    {
        return false;
    }
    EnsureRoot(root);
    uint32_t current = RootNode(root);
    uint32_t position = 0;
    for (;;)
    {
        while (path[position] == '/')
        {
            ++position;
        }
        uint32_t length = 0;
        while (path[position + length] != '\0' && path[position + length] != '/')
        {
            ++length;
        }
        if (length == 0 || !nodes[current].directory ||
            !FindChild(current, path + position, length, &current))
        {
            return false;
        }
        position += length;
        if (nodes[current].link)
        {
            *link_end = position;
            return true;
        }
    }
}

uint64_t UsedBytes()
{
    return used_bytes;
}

void Reset()
{
    for (Node& node : nodes)
    {
        if (node.used && !node.directory)
        {
            FreeData(node);
        }
        node = {};
    }
    used_bytes = 0;
}

} // namespace FeatherOS::Tmpfs
