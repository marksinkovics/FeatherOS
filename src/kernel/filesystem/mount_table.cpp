#include "mount_table.h"

namespace FeatherOS::VFS
{
namespace
{

uint32_t Length(const char* text)
{
    uint32_t length = 0;
    while (text[length] != '\0')
    {
        ++length;
    }
    return length;
}

bool SamePrefix(const char* left, const char* right, uint32_t length)
{
    for (uint32_t index = 0; index < length; ++index)
    {
        if (left[index] != right[index])
        {
            return false;
        }
    }
    return true;
}

bool SameText(const char* left, const char* right)
{
    const uint32_t length = Length(left);
    return Length(right) == length && SamePrefix(left, right, length);
}

// How many characters of path the mount point covers, or 0 if it does not hold path.
uint32_t Coverage(const char* point, const char* path)
{
    if (point[1] == '\0')
    {
        return 1;
    }
    const uint32_t length = Length(point);
    if (!SamePrefix(point, path, length) || (path[length] != '\0' && path[length] != '/'))
    {
        return 0;
    }
    return length;
}

// The length of directory without trailing '/'s, so the root is 0.
uint32_t DirectoryLength(const char* directory)
{
    uint32_t length = Length(directory);
    while (length > 0 && directory[length - 1] == '/')
    {
        --length;
    }
    return length;
}

// Where the point's last component starts, if it lies directly inside directory.
const char* ChildName(const char* point, const char* directory, uint32_t directory_length)
{
    if (point[1] == '\0')
    {
        return nullptr;
    }
    uint32_t last_separator = 0;
    for (uint32_t index = 0; point[index] != '\0'; ++index)
    {
        if (point[index] == '/')
        {
            last_separator = index;
        }
    }
    if (last_separator != directory_length || !SamePrefix(point, directory, directory_length))
    {
        return nullptr;
    }
    return point + last_separator + 1;
}

// Whether point lies inside base, not at it.
bool IsInside(const char* point, const char* base)
{
    if (SameText(point, base))
    {
        return false;
    }
    return base[1] == '\0' || Coverage(base, point) != 0;
}

} // namespace

bool MountTable::IsValidPoint(const char* point)
{
    if (point == nullptr || point[0] != '/')
    {
        return false;
    }
    const uint32_t length = Length(point);
    if (length >= Path::MaximumLength || (length > 1 && point[length - 1] == '/'))
    {
        return false;
    }
    for (uint32_t start = 1; start < length;)
    {
        uint32_t end = start;
        while (end < length && point[end] != '/')
        {
            ++end;
        }
        const uint32_t component = end - start;
        if (component == 0 || (component == 1 && point[start] == '.') ||
            (component == 2 && point[start] == '.' && point[start + 1] == '.'))
        {
            return false;
        }
        start = end + 1;
    }
    return true;
}

bool MountTable::Add(const char* point, Filesystem::Driver* driver)
{
    if (driver == nullptr || !IsValidPoint(point) || count == Capacity || Find(point) != nullptr ||
        Holds(driver))
    {
        return false;
    }
    Entry& entry = entries[count++];
    uint32_t index = 0;
    for (; point[index] != '\0'; ++index)
    {
        entry.point[index] = point[index];
    }
    entry.point[index] = '\0';
    entry.driver = driver;
    entry.users = 0;
    entry.unmounting = false;
    return true;
}

bool MountTable::Remove(const char* point)
{
    const int32_t found = IndexOf(point);
    if (found < 0)
    {
        return false;
    }
    for (auto next = static_cast<uint32_t>(found) + 1; next < count; ++next)
    {
        entries[next - 1] = entries[next];
    }
    entries[--count] = {};
    return true;
}

Filesystem::Driver* MountTable::Find(const char* point) const
{
    const int32_t found = IndexOf(point);
    return found < 0 ? nullptr : entries[found].driver;
}

bool MountTable::Holds(const Filesystem::Driver* driver) const
{
    for (uint32_t index = 0; index < count; ++index)
    {
        if (entries[index].driver == driver)
        {
            return true;
        }
    }
    return false;
}

uint32_t MountTable::Count() const
{
    return count;
}

bool MountTable::At(uint32_t index, const char** point, Filesystem::Driver** driver) const
{
    if (index >= count || point == nullptr || driver == nullptr)
    {
        return false;
    }
    *point = entries[index].point;
    *driver = entries[index].driver;
    return true;
}

Filesystem::Driver* MountTable::Resolve(const char* path, const char** inner) const
{
    const int32_t found = ResolveIndex(path, inner);
    return found < 0 ? nullptr : entries[found].driver;
}

Filesystem::Driver* MountTable::Acquire(const char* path, const char** inner)
{
    const int32_t found = ResolveIndex(path, inner);
    if (found < 0 || entries[found].unmounting)
    {
        return nullptr;
    }
    ++entries[found].users;
    return entries[found].driver;
}

Filesystem::Driver* MountTable::AcquirePoint(const char* point)
{
    const int32_t found = IndexOf(point);
    if (found < 0 || entries[found].unmounting)
    {
        return nullptr;
    }
    ++entries[found].users;
    return entries[found].driver;
}

void MountTable::Release(const Filesystem::Driver* driver)
{
    for (uint32_t index = 0; index < count; ++index)
    {
        if (entries[index].driver == driver && entries[index].users > 0)
        {
            --entries[index].users;
            return;
        }
    }
}

MountResult MountTable::BeginUnmount(const char* point, Filesystem::Driver** driver)
{
    const int32_t found = IndexOf(point);
    if (found < 0 || driver == nullptr)
    {
        return MountResult::NotFound;
    }
    Entry& entry = entries[found];
    if (entry.users > 0 || entry.unmounting)
    {
        return MountResult::Busy;
    }
    for (uint32_t index = 0; index < count; ++index)
    {
        if (IsInside(entries[index].point, entry.point))
        {
            return MountResult::Busy;
        }
    }
    entry.unmounting = true;
    *driver = entry.driver;
    return MountResult::Done;
}

void MountTable::FinishUnmount(const char* point)
{
    Remove(point);
}

void MountTable::CancelUnmount(const char* point)
{
    const int32_t found = IndexOf(point);
    if (found >= 0)
    {
        entries[found].unmounting = false;
    }
}

bool MountTable::ChildAt(const char* directory, uint32_t index,
                         Filesystem::DirectoryEntry* entry) const
{
    if (directory == nullptr || entry == nullptr || directory[0] != '/')
    {
        return false;
    }
    const uint32_t directory_length = DirectoryLength(directory);
    uint32_t seen = 0;
    for (uint32_t position = 0; position < count; ++position)
    {
        const char* name = ChildName(entries[position].point, directory, directory_length);
        if (name == nullptr || seen++ != index)
        {
            continue;
        }
        uint32_t length = 0;
        for (; name[length] != '\0'; ++length)
        {
            entry->name[length] = name[length];
        }
        entry->name[length] = '\0';
        entry->directory = true;
        entry->size = 0;
        return true;
    }
    return false;
}

uint32_t MountTable::ChildCount(const char* directory) const
{
    if (directory == nullptr || directory[0] != '/')
    {
        return 0;
    }
    const uint32_t directory_length = DirectoryLength(directory);
    uint32_t children = 0;
    for (uint32_t position = 0; position < count; ++position)
    {
        if (ChildName(entries[position].point, directory, directory_length) != nullptr)
        {
            ++children;
        }
    }
    return children;
}

int32_t MountTable::IndexOf(const char* point) const
{
    if (point == nullptr)
    {
        return -1;
    }
    for (uint32_t index = 0; index < count; ++index)
    {
        if (SameText(entries[index].point, point))
        {
            return static_cast<int32_t>(index);
        }
    }
    return -1;
}

int32_t MountTable::ResolveIndex(const char* path, const char** inner) const
{
    if (path == nullptr || inner == nullptr || path[0] != '/')
    {
        return -1;
    }
    int32_t found = -1;
    uint32_t best = 0;
    for (uint32_t index = 0; index < count; ++index)
    {
        const uint32_t covered = Coverage(entries[index].point, path);
        if (covered > best)
        {
            best = covered;
            found = static_cast<int32_t>(index);
        }
    }
    if (found < 0)
    {
        return -1;
    }
    const char* rest = path + best;
    while (*rest == '/')
    {
        ++rest;
    }
    *inner = rest;
    return found;
}

} // namespace FeatherOS::VFS
