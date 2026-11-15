#ifndef FEATHEROS_FILESYSTEM_CORE_DRIVER_H
#define FEATHEROS_FILESYSTEM_CORE_DRIVER_H

#include "kernel/filesystem/filesystem.h"

#include "fs/filesystem.h"
#include "fs/partition.h"

#include <stddef.h>
#include <stdint.h>
#include <string_view>

namespace FeatherOS::Filesystem
{

// A filesystem from the shared core (src/fs/core/) as a VFS driver. Paths are walked one
// component at a time with the core's Lookup, any core error becomes a failure, and a handle's
// node ID is the core's. Like the core's drivers, it expects one operation at a time, which the
// file system lock in files.cpp provides.
class CoreDriver final : public Driver
{
public:
    constexpr explicit CoreDriver(Properties properties) : Driver(properties) {}

    // Mounts the first partition of disk that type accepts, once the filesystem mounted before
    // is unmounted. Read-only properties mount it read-only.
    bool Mount(FeatherDisk::FilesystemType& type, FeatherDisk::BlockDevice& disk);
    // The mounted partition's place in Partition::Find's list, from 1; 0 with nothing mounted.
    size_t PartitionNumber() const;
    bool Statistics(FeatherDisk::VolumeStatistics* statistics);

    bool Open(const char* path, Node* node, uint64_t* size) override;
    bool Stat(const char* path, FileInfo* info) override;
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
    bool Sync() override;

private:
    // Walks every component of path but the last, which *leaf receives; "" (and "/" repeated)
    // leave *leaf empty, naming the root.
    FeatherDisk::Result<FeatherDisk::NodeId> ResolveParent(const char* path,
                                                           std::string_view* leaf);
    FeatherDisk::Result<FeatherDisk::NodeId> Resolve(const char* path);
    bool IsKind(FeatherDisk::NodeId node, FeatherDisk::NodeType type);

    FeatherDisk::Partition::Volume volume;
    FeatherDisk::FilesystemType* type = nullptr;
    FeatherDisk::Filesystem* filesystem = nullptr;
    size_t partition_number = 0;
};

} // namespace FeatherOS::Filesystem

#endif
