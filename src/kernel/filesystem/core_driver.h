#ifndef FEATHEROS_FILESYSTEM_CORE_DRIVER_H
#define FEATHEROS_FILESYSTEM_CORE_DRIVER_H

#include "kernel/filesystem/filesystem.h"

#include "fs/filesystem.h"
#include "fs/page_cache.h"
#include "fs/partition.h"
#include "kernel/storage/disk.h"
#include "kernel/sync/spinlock.h"
#include "user/thread_safety.h"

#include <atomic>
#include <stddef.h>
#include <span>
#include <stdint.h>
#include <string_view>

namespace FeatherOS::Filesystem
{

// The User::SyscallError code for a core error, as Linux's errno for the same failure.
uint64_t SyscallError(FeatherDisk::Error error);

struct CoreOptions
{
    bool periodic_sync = false;
    bool close_on_detach = false;
};

struct CoreKind
{
    FeatherDisk::FilesystemType& (*type)();
    const char* name;
    CoreOptions options;
};

// A filesystem from the shared core (src/fs/core/) as a VFS driver. Paths are walked one
// component at a time with the core's Lookup, a core error becomes a failure that LastError
// explains, and a handle's node ID is the core's. Like the core's drivers, it expects one
// operation at a time, which the driver's own volume lock provides. Each driver serves one
// partition and all its state is its own.
class CoreDriver final : public Driver
{
public:
    constexpr CoreDriver() : Driver({.read_only = false, .tracked = true, .devices = false}) {}

    // Opens the filesystem on range, read-only in the core, as the first of kinds whose probe
    // accepts the volume and whose mount succeeds; the options and TypeName follow that kind. A
    // VFS mount asking for read-write reopens the core, as PrepareMount does. The filesystem
    // opened before is unmounted first; false, with the driver as it was, if that fails. Every
    // read and write of the volume goes through its CachedDevice, which uses cache, or none when
    // that is null. The driver asks the disk, which its slot keeps alive, at each read-write
    // mount whether the volume may be written (see MediaAllowsWrites).
    bool OpenPartition(std::span<const CoreKind> kinds, Storage::Disk& disk,
                       const FeatherDisk::Partition::Range& range, FeatherDisk::PageCache* cache);
    // Unmounts the core, drops the volume's cached pages and leaves the driver without a volume;
    // false, unchanged, if the core cannot unmount.
    bool Forget();
    // Whether range shares sectors of disk with the volume this driver holds; not under the
    // volume lock. Ranges on another disk never overlap.
    bool Overlaps(const FeatherDisk::BlockDevice& disk,
                  const FeatherDisk::Partition::Range& other) const;
    struct Summary
    {
        uint64_t first_sector;
        uint64_t sector_count;
        size_t number;
        const char* type;
    };
    // The volume held on disk; false for any other; not under the volume lock.
    bool Summarize(const FeatherDisk::BlockDevice& disk, Summary* summary) const;
    // The disk is gone: from now on every operation fails with EIO before reaching the core and
    // the mount shows read-only. Any task, no lock.
    void MarkDead();
    bool Dead() const;
    // For a slot about to serve another volume.
    void ClearDead();
    // Frees the core's filesystem with no device I/O and leaves the driver without a volume.
    void Discard();
    // Drops the volume's cached pages, for tests that change the disk below the cache.
    void InvalidateCache();
    // The kind's registry name, "ext4" for example; null without a volume.
    const char* TypeName() const;
    // The volume's partition number (Partition::Range::number, from 1); 0 with nothing open.
    size_t PartitionNumber() const;
    bool Statistics(FeatherDisk::VolumeStatistics* statistics);

    bool Open(const char* path, Node* node, uint64_t* size) override;
    bool Stat(const char* path, FileInfo* info) override;
    bool StatNode(const Node& node, FileInfo* info) override;
    bool Read(const Node& node, uint64_t size, uint64_t offset, void* buffer, uint64_t length,
              uint64_t* bytes_read) override;
    bool Create(const char* path, Node* node, uint64_t* size) override;
    bool Write(Node* node, uint64_t* size, uint64_t offset, const void* buffer, uint64_t length,
               uint64_t* bytes_written) override;
    // Only shrinks, as the VFS promises.
    bool Truncate(Node* node, uint64_t* size, uint64_t new_size) override;
    bool Remove(const char* path) override;
    bool MakeDirectory(const char* path) override;
    bool RemoveDirectory(const char* path) override;
    // Onto a missing name, or onto the same node under another case; files.cpp removes a file
    // that a rename replaces first.
    bool Rename(const char* from, const char* to) override;
    bool IsDirectory(const char* path) override;
    bool ReadDirectory(const char* path, uint32_t index, DirectoryEntry* entry) override;
    // What the mounted filesystem's capabilities say, writable or not.
    bool SupportsLinks(LinkKind kind) override;
    bool FindLink(const char* path, uint32_t* link_end) override;
    bool ReadLink(const char* path, char* buffer, uint32_t capacity, uint32_t* length) override;
    bool SymbolicLink(const char* path, const char* target) override;
    bool Link(const char* from, const char* to) override;
    bool Sync() override;
    // Unmounts the core, which closes a journal, and reopens it read-only, for the volumes that
    // ask for it; a plain Sync for the rest.
    bool Detach() override;
    bool SyncsInBackground() const override;
    uint64_t LastError() override;
    // True while the volume is mounted read-only with a journal that needs recovery, which a
    // read-write remount would replay.
    bool RemountReplays() override;

protected:
    bool OpenForMount(MountMode mode, bool* writable) override;
    // Read-only makes the core refuse writes in place; read-write reopens a core that was
    // read-only as a fresh mount does. A core that refuses writes after being asked for them stays
    // as it is. A core whose reopen failed has no filesystem: its operations fail with EIO, it
    // refuses writes, and the next mount or remount opens it again.
    bool ReopenForRemount(MountMode mode, bool* writable) override;
    bool RefusesWrites() const override;

private:
    bool Fail(FeatherDisk::Error error);
    void SetIdentity(const FeatherDisk::BlockDevice* disk, const FeatherDisk::Partition::Range& r,
                     const char* type_name);
    bool DetachCore();
    bool CoreReadOnly(MountMode mode) const;
    // False, logging why, for a write-protected disk and for a FAT16 volume of a removable disk
    // whose status bits say it was not cleanly unmounted: it mounts read-only, remounting
    // read-write gives EROFS, and the core never writes the bits.
    bool MediaAllowsWrites();
    // Walks every component of path but the last, which *leaf receives; "" (and "/" repeated)
    // leave *leaf empty, naming the root.
    FeatherDisk::Result<FeatherDisk::NodeId> ResolveParent(const char* path,
                                                           std::string_view* leaf);
    FeatherDisk::Result<FeatherDisk::NodeId> Resolve(const char* path);
    bool IsKind(FeatherDisk::NodeId node, FeatherDisk::NodeType type);

    CoreOptions options;
    const char* name = nullptr;
    FeatherDisk::Partition::Volume volume;
    FeatherDisk::CachedDevice cached;
    FeatherDisk::Partition::Range range = {};
    const Storage::Disk* media = nullptr;
    FeatherDisk::FilesystemType* type = nullptr;
    FeatherDisk::Filesystem* filesystem = nullptr;
    size_t partition_number = 0;
    // What the core was asked for, which a volume that refuses writes may not have given.
    bool core_read_only = true;
    // Written and read under the volume lock.
    uint64_t last_error = User::SyscallErrorFailed;
    std::atomic<bool> dead = false;
    // What Overlaps and Summarize read without the volume lock.
    struct Identity
    {
        const FeatherDisk::BlockDevice* disk = nullptr;
        FeatherDisk::Partition::Range range = {};
        const char* type_name = nullptr;
    };
    mutable Sync::SpinLock identity_lock;
    Identity identity GUARDED_BY(identity_lock);
};

} // namespace FeatherOS::Filesystem

#endif
