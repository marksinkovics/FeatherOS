#ifndef FEATHEROS_FILESYSTEM_MOUNT_TABLE_H
#define FEATHEROS_FILESYSTEM_MOUNT_TABLE_H

#include "kernel/filesystem/filesystem.h"
#include "kernel/filesystem/path.h"

#include <stdint.h>

namespace FeatherOS::VFS
{

// Mount points are absolute paths without "." or "..", repeated '/' or a trailing '/', such as
// "/" or "/tmp". The table needs no allocation, and its caller provides the locking.
class MountTable
{
public:
    static constexpr uint32_t Capacity = 16;

    // Fails if the point is malformed or taken, or the table is full.
    bool Add(const char* point, Filesystem::Driver* driver);
    bool Remove(const char* point);
    Filesystem::Driver* Find(const char* point) const;
    // The mount holding an absolute path: the longest mount point that is the path or is followed
    // in it by '/'. *inner receives the rest of the path without its leading '/'s ("" for the
    // mount point itself). Null if no mount point matches.
    Filesystem::Driver* Resolve(const char* path, const char** inner) const;
    // The index-th mount point directly inside directory, in mount order, as a directory entry;
    // false past the last one.
    bool ChildAt(const char* directory, uint32_t index, Filesystem::DirectoryEntry* entry) const;
    uint32_t ChildCount(const char* directory) const;

private:
    struct Entry
    {
        char point[Path::MaximumLength];
        Filesystem::Driver* driver;
    };

    Entry entries[Capacity] = {};
    uint32_t count = 0;
};

} // namespace FeatherOS::VFS

#endif
