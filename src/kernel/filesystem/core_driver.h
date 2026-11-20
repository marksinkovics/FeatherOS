#ifndef FEATHEROS_FILESYSTEM_CORE_DRIVER_H
#define FEATHEROS_FILESYSTEM_CORE_DRIVER_H

#include "kernel/filesystem/filesystem.h"

#include "fs/filesystem.h"
#include "fs/partition.h"

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
    // opened before is unmounted first; false, with the driver as it was, if that fails.
    bool OpenPartition(std::span<const CoreKind> kinds, FeatherDisk::BlockDevice& disk,
                       const FeatherDisk::Partition::Range& range);
    // Unmounts the core and leaves the driver without a volume; false, unchanged, if the core
    // cannot unmount.
    bool Forget();
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
    bool CoreReadOnly(MountMode mode) const;
    // Walks every component of path but the last, which *leaf receives; "" (and "/" repeated)
    // leave *leaf empty, naming the root.
    FeatherDisk::Result<FeatherDisk::NodeId> ResolveParent(const char* path,
                                                           std::string_view* leaf);
    FeatherDisk::Result<FeatherDisk::NodeId> Resolve(const char* path);
    bool IsKind(FeatherDisk::NodeId node, FeatherDisk::NodeType type);

    CoreOptions options;
    const char* name = nullptr;
    FeatherDisk::Partition::Volume volume;
    FeatherDisk::FilesystemType* type = nullptr;
    FeatherDisk::Filesystem* filesystem = nullptr;
    size_t partition_number = 0;
    // What the core was asked for, which a volume that refuses writes may not have given.
    bool core_read_only = true;
    // Written and read under the volume lock.
    uint64_t last_error = User::SyscallErrorFailed;
};

} // namespace FeatherOS::Filesystem

#endif
