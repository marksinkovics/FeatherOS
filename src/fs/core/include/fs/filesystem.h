// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_FILESYSTEM_H
#define FEATHERDISK_FILESYSTEM_H

#include "fs/block_device.h"
#include "fs/error.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace FeatherDisk
{

// Opaque to callers and unique within a mounted volume. An ID stays valid until its node is
// removed or renamed; FAT, which has no inodes, derives IDs from where the entry lives.
using NodeId = std::uint64_t;

// The longest name, in bytes of UTF-8, that any driver hands out or accepts.
constexpr std::size_t MaximumNameLength = 255;
// PATH_MAX, terminator included: a symbolic link's target is at most one byte shorter.
constexpr std::size_t MaximumPathLength = 1024;

enum class NodeType : std::uint8_t
{
    File,
    Directory,
    SymbolicLink,
    // Device nodes, FIFOs and sockets, which ext2 can store.
    Special,
};

// Since 1970-01-01 UTC.
struct Timestamp
{
    std::int64_t seconds;
    std::uint32_t nanoseconds;
};

struct Attributes
{
    NodeId node;
    NodeType type;
    std::uint64_t size;
    // Permission bits (07777); a filesystem without permissions reports what it allows.
    std::uint32_t mode;
    std::uint32_t owner;
    std::uint32_t group;
    std::uint32_t link_count;
    Timestamp access_time;
    Timestamp modify_time;
    Timestamp change_time;
};

// Only the fields that are set change; a filesystem that cannot store one fails with
// Error::Unsupported and changes nothing.
struct AttributeChanges
{
    std::optional<std::uint32_t> mode;
    std::optional<std::uint32_t> owner;
    std::optional<std::uint32_t> group;
    std::optional<Timestamp> access_time;
    std::optional<Timestamp> modify_time;
};

struct DirectoryEntry
{
    NodeId node;
    NodeType type;
    std::uint16_t name_length;
    // UTF-8, not null-terminated.
    std::array<char, MaximumNameLength> name;
};

struct Capabilities
{
    bool writable;
    bool symbolic_links;
    bool hard_links;
    // Mode, owner and group are stored.
    bool permissions;
    bool case_sensitive;
    std::uint32_t maximum_name_length;
    std::uint64_t maximum_file_size;
};

struct VolumeStatistics
{
    std::uint32_t block_size;
    std::uint64_t total_blocks;
    std::uint64_t free_blocks;
    // Zero for a filesystem without a fixed number of nodes, such as FAT.
    std::uint64_t total_nodes;
    std::uint64_t free_nodes;
};

struct MountOptions
{
    bool read_only;
};

// A mounted volume. Names are single path components; walking paths is the host's job. The
// operations a driver leaves out fail with Error::Unsupported, except Sync, which succeeds.
class Filesystem
{
public:
    Filesystem() = default;
    Filesystem(const Filesystem&) = delete;
    Filesystem& operator=(const Filesystem&) = delete;

    virtual Capabilities GetCapabilities() const = 0;
    virtual NodeId Root() const = 0;
    virtual Result<VolumeStatistics> Statistics() = 0;

    virtual Result<NodeId> Lookup(NodeId directory, std::string_view name) = 0;
    // Starts at cookie 0 and advances it; false after the last entry. "." and ".." are left out.
    virtual Result<bool> ReadDirectory(NodeId directory, std::uint64_t& cookie,
                                       DirectoryEntry& entry) = 0;
    virtual Result<Attributes> GetAttributes(NodeId node) = 0;
    virtual Status SetAttributes(NodeId node, const AttributeChanges& changes);

    // Fewer bytes than asked only at the end of the file.
    virtual Result<std::size_t> Read(NodeId file, std::uint64_t offset,
                                     std::span<std::uint8_t> buffer) = 0;
    // Writing past the end fills the gap with zeros. Fewer bytes than given means the volume
    // filled up, with the bytes before that written.
    virtual Result<std::size_t> Write(NodeId file, std::uint64_t offset,
                                      std::span<const std::uint8_t> data);
    // Grows with zeros or shrinks, freeing what the file no longer needs.
    virtual Status Truncate(NodeId file, std::uint64_t size);

    virtual Result<NodeId> Create(NodeId directory, std::string_view name, std::uint32_t mode);
    virtual Result<NodeId> MakeDirectory(NodeId directory, std::string_view name,
                                         std::uint32_t mode);
    // A file, symbolic link or special node; directories go through RemoveDirectory.
    virtual Status Remove(NodeId directory, std::string_view name);
    // Only an empty directory.
    virtual Status RemoveDirectory(NodeId directory, std::string_view name);
    // Replaces an existing target of the same kind, as POSIX rename does, and refuses to move a
    // directory inside itself. Renaming onto another name of the same node changes nothing.
    virtual Status Rename(NodeId from_directory, std::string_view from_name, NodeId to_directory,
                          std::string_view to_name);
    // A hard link: Error::NotPermitted for a directory, Error::TooManyLinks past the
    // filesystem's limit.
    virtual Status Link(NodeId node, NodeId directory, std::string_view name);
    // target is stored as given and need not exist; Error::NameTooLong from MaximumPathLength on.
    virtual Result<NodeId> SymbolicLink(NodeId directory, std::string_view name,
                                        std::string_view target);
    virtual Result<std::size_t> ReadLink(NodeId link, std::span<char> buffer);

    // Writes everything the driver holds back and flushes the device.
    virtual Status Sync();

    // Makes everything durable, leaves the medium as an unmounted volume would be and refuses
    // writes from then on; reads go on. An instance that writes nothing (mounted read-only, fell
    // back, aborted) writes nothing and succeeds. On failure the filesystem is as Sync or the
    // journal's close left it. Default: Error::Unsupported.
    virtual Status MakeReadOnly();

protected:
    // Only the filesystem's own type frees it, in FilesystemType::Unmount.
    ~Filesystem() = default;
};

// One kind of filesystem, such as FAT16 or ext2: a driver.
class FilesystemType
{
public:
    FilesystemType() = default;
    FilesystemType(const FilesystemType&) = delete;
    FilesystemType& operator=(const FilesystemType&) = delete;

    virtual std::string_view Name() const = 0;
    // Looks only at the superblock or boot sector and changes nothing.
    virtual bool Probe(BlockDevice& volume) = 0;
    // The filesystem uses volume until it is unmounted, and its memory comes from Host.
    virtual Result<Filesystem*> Mount(BlockDevice& volume, MountOptions options) = 0;
    // Syncs and then frees the filesystem. If syncing fails, it stays mounted.
    virtual Status Unmount(Filesystem& filesystem) = 0;
    // Frees the filesystem without any device I/O, for a volume whose device is gone: what was not
    // written is lost.
    virtual void Discard(Filesystem& filesystem) = 0;

protected:
    ~FilesystemType() = default;
};

} // namespace FeatherDisk

#endif
