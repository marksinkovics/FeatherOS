#ifndef FEATHEROS_FILESYSTEM_MOUNT_TABLE_H
#define FEATHEROS_FILESYSTEM_MOUNT_TABLE_H

#include "kernel/filesystem/filesystem.h"
#include "kernel/filesystem/path.h"

#include <stdint.h>

namespace FeatherOS::VFS
{

enum class MountResult : uint8_t
{
    Done,
    // Not a mount point, or for a mount an unknown source.
    NotFound,
    // Open files, operations in progress or mount points inside it; or, for a mount, the point
    // is taken or the instance is mounted elsewhere.
    Busy,
    Invalid,
    UnknownType,
    Full,
    // The driver could not sync, so the mount stays.
    Failed,
    // A remount to read-write of what refuses writes.
    ReadOnly,
    // A volume whose disk is gone.
    NoDevice,
};

// Mount points are absolute paths without "." or "..", repeated '/' or a trailing '/', such as
// "/" or "/tmp". A driver instance is mounted at one point at most. The table needs no
// allocation, and its caller provides the locking.
//
// A driver is used only between Acquire and Release, and BeginUnmount succeeds only while its
// mount has no such users and then marks it, so Acquire fails for it from then on. The caller can
// therefore drop its lock to sync the driver and still be sure that nothing starts using it before
// FinishUnmount.
class MountTable
{
public:
    static constexpr uint32_t Capacity = 16;

    static bool IsValidPoint(const char* point);

    // Fails if the point is malformed or taken, the driver is mounted, or the table is full.
    bool Add(const char* point, Filesystem::Driver* driver);
    // Removes the mount whatever its users, for replacing the root at startup.
    bool Remove(const char* point);
    Filesystem::Driver* Find(const char* point) const;
    bool Holds(const Filesystem::Driver* driver) const;
    uint32_t Count() const;
    // The index-th mount in mount order; false past the last one.
    bool At(uint32_t index, const char** point, Filesystem::Driver** driver) const;
    // The mount holding an absolute path: the longest mount point that is the path or is followed
    // in it by '/'. *inner receives the rest of the path without its leading '/'s ("" for the
    // mount point itself). Null if no mount point matches.
    Filesystem::Driver* Resolve(const char* path, const char** inner) const;
    // Resolve, counting a user of the mount until Release; null while it is being unmounted.
    Filesystem::Driver* Acquire(const char* path, const char** inner);
    // The driver mounted exactly at point, counted as Acquire does.
    Filesystem::Driver* AcquirePoint(const char* point);
    void Release(const Filesystem::Driver* driver);
    // Busy while the mount has users or another mount point lies inside it; on Done the mount is
    // marked and *driver receives its driver.
    MountResult BeginUnmount(const char* point, Filesystem::Driver** driver);
    void FinishUnmount(const char* point);
    void CancelUnmount(const char* point);
    // The index-th mount point directly inside directory, in mount order, as a directory entry;
    // false past the last one. *driver receives its driver, counted as Acquire does, or null
    // while it is being unmounted.
    bool AcquireChild(const char* directory, uint32_t index, Filesystem::DirectoryEntry* entry,
                      Filesystem::Driver** driver);
    uint32_t ChildCount(const char* directory) const;
    // The number each mount gets when it is added, unique until the table is destroyed, for
    // st_dev; 0 for a driver that is not mounted.
    uint64_t DeviceOf(const Filesystem::Driver* driver) const;

private:
    struct Entry
    {
        char point[Path::MaximumLength];
        Filesystem::Driver* driver;
        uint64_t device;
        uint32_t users;
        bool unmounting;
    };

    int32_t IndexOf(const char* point) const;
    int32_t ResolveIndex(const char* path, const char** inner) const;

    Entry entries[Capacity] = {};
    uint32_t count = 0;
    uint64_t next_device = 1;
};

} // namespace FeatherOS::VFS

#endif
