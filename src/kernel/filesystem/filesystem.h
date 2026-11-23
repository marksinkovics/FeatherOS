#ifndef FEATHEROS_FILESYSTEM_FILESYSTEM_H
#define FEATHEROS_FILESYSTEM_FILESYSTEM_H

#include "kernel/sync/mutex.h"
#include "user/syscall_abi.h"

#include <atomic>
#include <stdint.h>

// The interface between the VFS and a mounted filesystem. Paths reaching a driver are relative to
// its mount point, such as "docs/a.txt", with "" for its root.
namespace FeatherOS::Filesystem
{

struct DirectoryEntry
{
    // A FAT long name if the entry has one, otherwise "NAME.EXT", and a terminator.
    char name[User::MaximumNameLength + 1];
    // The mode's type bits (User::ModeDirectory and the rest); 0 if unknown.
    uint32_t type;
    // As FileInfo::node; 0 if unknown.
    uint64_t node;
    // 0 for directories.
    uint64_t size;
};

// Since 1970-01-01 UTC.
struct Timestamp
{
    int64_t seconds;
    uint32_t nanoseconds;
};

struct FileInfo
{
    // Unique within the filesystem while the file exists.
    uint64_t node;
    uint64_t size;
    // Type and permission bits, as st_mode holds them.
    uint32_t mode;
    uint32_t link_count;
    uint32_t owner;
    uint32_t group;
    Timestamp access_time;
    Timestamp modify_time;
    Timestamp change_time;
    // The mount's number, which the VFS fills in; drivers leave it.
    uint64_t device;
};

// An open file as its driver identifies it; the VFS never looks inside. The ID stays the same for
// every handle to the same file while it exists, so it keys the open-file rules.
struct Node
{
    uint64_t id;
    uint64_t payload;
};

enum class LinkKind : uint8_t
{
    Symbolic,
    Hard,
};

enum class MountMode : uint8_t
{
    ReadWrite,
    // Nothing on the mount changes, whatever the driver could write.
    ReadOnly,
};

struct Properties
{
    // Nothing on it can be created, written or removed.
    bool read_only;
    // Its open files are tracked, so a file is not removed or written by two handles at once.
    bool tracked;
    // Its files are devices: they have no size, accept any offset and never move it.
    bool devices;
};

// Operations a driver leaves out fail.
class Driver
{
public:
    constexpr explicit Driver(Properties properties)
        : properties(properties), mounted_read_only(properties.read_only)
    {
    }
    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;
    Driver(Driver&&) = delete;
    Driver& operator=(Driver&&) = delete;

    const Properties& Attributes() const
    {
        return properties;
    }

    // The VFS calls it before adding the driver to the tree; false refuses the mount. Read-only
    // properties keep it read-only whatever mode asks.
    bool PrepareMount(MountMode mode);
    // Read-only by its properties, by how it was last mounted, or because its filesystem refuses
    // writes. Other volumes' modes are read without their locks, as /proc/mounts does.
    bool IsMountedReadOnly() const
    {
        return mounted_read_only.load(std::memory_order_relaxed);
    }
    // Held by the VFS around every call on the driver, so one task at a time uses it; drivers that
    // share state share a lock.
    virtual Sync::VolumeMutex& VolumeLock();
    // Changes a mounted driver's mode as a fresh mount of that mode would set it. The VFS handles
    // read-only properties and devices first. false: LastError says why, SyscallErrorReadOnly when
    // read-write was asked of a medium that refuses writes, which leaves it mounted read-only.
    bool Remount(MountMode mode);
    // After any call that may have made the medium refuse writes: the mount is read-only from
    // then on.
    void RefreshMode();
    // A read-write Remount would replay a journal, changing what open files read.
    virtual bool RemountReplays();

    virtual bool Open(const char* path, Node* node, uint64_t* size);
    // Any file, directory or link, "" for the root; links are not followed.
    virtual bool Stat(const char* path, FileInfo* info);
    // An open file; the VFS fills in the size from the handle.
    virtual bool StatNode(const Node& node, FileInfo* info);
    virtual bool Read(const Node& node, uint64_t size, uint64_t offset, void* buffer,
                      uint64_t length, uint64_t* bytes_read);
    virtual void Close(const Node& node);
    virtual bool Create(const char* path, Node* node, uint64_t* size);
    // offset is at most *size. Updates *size; *bytes_written may be short when space runs out.
    virtual bool Write(Node* node, uint64_t* size, uint64_t offset, const void* buffer,
                       uint64_t length, uint64_t* bytes_written);
    virtual bool Truncate(Node* node, uint64_t* size, uint64_t new_size);
    virtual bool Remove(const char* path);
    virtual bool MakeDirectory(const char* path);
    virtual bool RemoveDirectory(const char* path);
    // Both paths are on this filesystem. An existing to of the same kind (a directory only if
    // empty) is replaced as part of the same change.
    virtual bool Rename(const char* from, const char* to);
    virtual bool IsDirectory(const char* path);
    virtual bool ReadDirectory(const char* path, uint32_t index, DirectoryEntry* entry);
    virtual bool IsReadOnly(const char* path);
    // Whether the filesystem can hold this kind of link at all, written or not.
    virtual bool SupportsLinks(LinkKind kind);
    // For the VFS path walk: the end of the first component of path that is a symbolic link,
    // walking from the root and stopping at a missing component or a file. Every component is a
    // plain name, without "." or "..".
    virtual bool FindLink(const char* path, uint32_t* link_end);
    // The target of the symbolic link at path into buffer, as much as capacity holds, without a
    // terminator; *length receives the target's whole length. Fails for anything but a link.
    virtual bool ReadLink(const char* path, char* buffer, uint32_t capacity, uint32_t* length);
    // A new symbolic link at path holding target as given; path must not exist.
    virtual bool SymbolicLink(const char* path, const char* target);
    // A second name, to, for the file at from; to must not exist.
    virtual bool Link(const char* from, const char* to);
    // Makes everything written so far durable.
    virtual bool Sync();
    // Makes everything durable and leaves the medium as an unmounted volume would be. The VFS
    // calls it when it removes the mount and at shutdown. false: the mount stays (LastError says
    // why).
    virtual bool Detach();
    // Wants Sync every Writeback::IntervalSeconds while mounted.
    virtual bool SyncsInBackground() const;
    // The medium is gone: every call fails with SyscallErrorInputOutput, whatever the path.
    virtual bool Dead() const;
    // Why the call that just failed did, as a User::SyscallError code; drivers that cannot say
    // give SyscallErrorFailed.
    virtual uint64_t LastError();

protected:
    ~Driver() = default;

    // For drivers whose medium decides whether they take writes: reopens it for mode and sets
    // *writable. false leaves the driver unusable until it is opened again.
    virtual bool OpenForMount(MountMode mode, bool* writable);
    // Reopens or switches the medium for a remount to mode and sets *writable.
    virtual bool ReopenForRemount(MountMode mode, bool* writable);
    // The medium was asked to take writes and no longer does.
    virtual bool RefusesWrites() const;
    // For a medium that is gone: the mount shows read-only at once, from any task.
    void ForceReadOnly();

private:
    Properties properties;
    std::atomic<bool> mounted_read_only;
    Sync::VolumeMutex own_lock;
};

} // namespace FeatherOS::Filesystem

#endif
