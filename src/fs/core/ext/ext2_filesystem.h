// SPDX-License-Identifier: MIT OR Apache-2.0
// The filesystem behind Ext2Type(), Ext3Type() and Ext4Type(), shared by ext2.cpp (mounting and
// reading), extent.cpp (extent trees), recovery.cpp (journal replay), allocation.cpp (groups and
// bitmaps) and write.cpp (changes).
#ifndef FEATHERDISK_EXT_EXT2_FILESYSTEM_H
#define FEATHERDISK_EXT_EXT2_FILESYSTEM_H

#include "common/block_cache.h"
#include "common/host_buffer.h"
#include "ext/checksum.h"
#include "ext/directory.h"
#include "ext/inode.h"
#include "ext/superblock.h"
#include "fs/filesystem.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace FeatherDisk::Ext
{

template <typename T> std::unexpected<Error> Failure(const Result<T>& result)
{
    return std::unexpected(result.error());
}

std::uint64_t DivideRoundingUp(std::uint64_t value, std::uint64_t divisor);
// The buffers of one change, by use.
constexpr std::size_t DataBuffer = 0;
constexpr std::size_t ScratchBuffer = 1;
constexpr std::size_t BitmapBuffer = 2;
// Three: the indirect blocks a MapCursor holds, or those FreeTree walks.
constexpr std::size_t LevelBuffers = 3;
constexpr std::size_t ChangeBuffers = 6;

// A warning for the host's log: message, then a detail or value as eight hexadecimal digits.
void LogWarning(std::string_view message, std::string_view detail);
void LogWithValue(std::string_view message, std::uint32_t value);

// Block-sized buffers from the host, for one operation.
class Buffers
{
public:
    Buffers(std::uint32_t block_size, std::size_t count);

    bool Empty() const;
    std::span<std::uint8_t> operator[](std::size_t index);

private:
    HostBuffer memory;
    std::uint32_t size;
};

// An inode in use, of a type ext2 defines.
struct Node
{
    Inode inode;
    NodeType type;
};

constexpr std::size_t NoRecord = ~std::size_t{0};

// Where WalkDirectory found a record.
struct RecordPlace
{
    std::uint64_t physical;
    std::size_t offset;
    // The record before it in the same block, or NoRecord for the block's first.
    std::size_t previous;
    // The byte offset in the directory of the record after it: ReadDirectory's cookie.
    std::uint64_t next;
};

// count logical blocks from logical on: on consecutive blocks from physical on, or zeros when
// physical is 0 (holes and unwritten extents). count is at least 1.
struct BlockRun
{
    std::uint64_t logical;
    std::uint64_t physical;
    std::uint64_t count;
};

// The way to a file's logical block: i_block[root], then index[level] in each of depth indirect
// blocks.
struct BlockPath
{
    std::uint32_t root;
    std::uint32_t depth;
    std::array<std::uint32_t, 3> index;
};

// The indirect blocks on the way to the block a write maps, kept while consecutive blocks share
// them. A changed one is written when the way leaves it, after the deeper ones it points to.
struct MapCursor
{
    struct Level
    {
        // 0 when the level holds nothing.
        std::uint32_t block;
        bool dirty;
        std::span<std::uint8_t> data;
    };
    std::array<Level, 3> levels;
    // Where the next block is looked for.
    std::uint32_t goal;
    // Indirect and data blocks allocated, for i_blocks.
    std::uint64_t allocated;
};

// What SearchDirectory found for a name.
struct NameSearch
{
    // The record naming it, if any, with its inode and filetype byte.
    std::optional<RecordPlace> place;
    std::uint32_t inode;
    std::uint8_t file_type;
    // Without such a record: the first record with room for the bytes asked for, or nullopt
    // when the directory needs a new block.
    std::optional<RecordPlace> room;
};

// Blocks going back to their bitmaps: one group's bitmap is held until a block of another group
// is freed or FinishFrees.
struct FreeCursor
{
    std::span<std::uint8_t> bitmap;
    std::uint32_t group;
    bool loaded;
    bool dirty;
};

// Changes (everything from SetAttributes to SymbolicLink, and Sync) must not run at the same time
// as any other operation on the mount; reads may run together. The host serialises them.
class Ext2Filesystem final : public Filesystem
{
public:
    Ext2Filesystem(BlockDevice& device, const Superblock& layout, bool mounted_read_only);
    ~Ext2Filesystem() = default;

    // Constructed in memory from Host::Allocate; the core has no <new> for the global form.
    static void* operator new(std::size_t size, void* place) noexcept;

    Status Initialize();
    // Copies the transactions the journal committed to their places, empties the journal and
    // clears needs_recovery (recovery.cpp). Mount runs it on a read-only instance for a mount
    // asked to write, then mounts the recovered volume.
    Status RecoverJournal();

    Capabilities GetCapabilities() const override;
    NodeId Root() const override;
    Result<VolumeStatistics> Statistics() override;
    Result<NodeId> Lookup(NodeId directory, std::string_view name) override;
    Result<bool> ReadDirectory(NodeId directory, std::uint64_t& cookie,
                               DirectoryEntry& entry) override;
    Result<Attributes> GetAttributes(NodeId node) override;
    Status SetAttributes(NodeId node, const AttributeChanges& changes) override;
    Result<std::size_t> Read(NodeId file, std::uint64_t offset,
                             std::span<std::uint8_t> buffer) override;
    Result<std::size_t> Write(NodeId file, std::uint64_t offset,
                              std::span<const std::uint8_t> data) override;
    Status Truncate(NodeId file, std::uint64_t size) override;
    Result<NodeId> Create(NodeId directory, std::string_view name, std::uint32_t mode) override;
    Result<NodeId> MakeDirectory(NodeId directory, std::string_view name,
                                 std::uint32_t mode) override;
    Status Remove(NodeId directory, std::string_view name) override;
    Status RemoveDirectory(NodeId directory, std::string_view name) override;
    Status Rename(NodeId from_directory, std::string_view from_name, NodeId to_directory,
                  std::string_view to_name) override;
    Status Link(NodeId node, NodeId directory, std::string_view name) override;
    Result<NodeId> SymbolicLink(NodeId directory, std::string_view name,
                                std::string_view target) override;
    Result<std::size_t> ReadLink(NodeId link, std::span<char> buffer) override;
    Status Sync() override;

private:
    struct InodePlace
    {
        std::uint64_t block;
        std::size_t offset;
    };

    // Reading (ext2.cpp).
    Status CheckDescriptorChecksums() const;
    std::unexpected<Error> Refused() const;
    bool IsDataBlock(std::uint64_t block) const;
    std::uint64_t MaximumFileSize() const;
    Result<InodePlace> LocateInode(NodeId node) const;
    // An inode past what its group ever used (with group checksums), which reads as free.
    bool IsUnusedInode(NodeId node) const;
    Result<Inode> LoadInode(NodeId node, std::span<std::uint8_t> scratch);
    Result<Node> LoadNode(NodeId node, std::span<std::uint8_t> scratch);
    Result<std::uint32_t> ReadPointer(std::uint32_t block, std::uint64_t index,
                                      std::span<std::uint8_t> scratch);
    std::optional<BlockPath> PathOf(std::uint64_t logical) const;
    // The block holding a file's logical block, or 0 for a hole, through the block map only.
    Result<std::uint32_t> MapBlock(const Inode& inode, std::uint64_t logical,
                                   std::span<std::uint8_t> scratch);
    // The run from logical on, through the block map (one block) or the extent tree.
    Result<BlockRun> MapRun(const Inode& inode, std::uint64_t logical,
                            std::span<std::uint8_t> scratch);
    // Extent trees (extent.cpp).
    Result<BlockRun> MapExtent(const Inode& inode, std::uint64_t logical,
                               std::span<std::uint8_t> scratch);
    // Calls visit(entry, place) for each record from byte start on, unused ones (inode 0)
    // included, until it returns true. block receives each directory block and still holds the
    // block of the record visit stopped at; scratch serves the rest.
    template <typename Visit>
    Result<bool> WalkDirectory(const Inode& directory, std::uint64_t start,
                               std::span<std::uint8_t> block, std::span<std::uint8_t> scratch,
                               Visit visit);
    bool DirectoryBlockMatches(const Inode& directory, std::uint64_t logical,
                               std::span<const std::uint8_t> block) const;
    Result<NodeType> EntryType(const RawEntry& entry, std::span<std::uint8_t> scratch);
    Result<std::size_t> ReadContents(const Inode& file, std::uint64_t offset,
                                     std::span<std::uint8_t> buffer,
                                     std::span<std::uint8_t> scratch);

    // Journal replay (recovery.cpp).
    struct JournalLog;
    enum class LogPass : std::uint8_t
    {
        // Finds the end of the log: the first transaction without a valid commit.
        Scan,
        Revoke,
        Replay,
    };
    // What the scan found wrong with the transaction it is in, which counts once it commits.
    struct TransactionCheck
    {
        // A block outside the volume, or a revoke count past its block.
        bool outside;
        // A descriptor, revoke or block checksum that does not match.
        bool mismatch;
    };
    Status OpenJournal(JournalLog& log, Buffers& buffers);
    Result<std::uint64_t> LogBlockPlace(JournalLog& log, std::uint32_t position,
                                        std::span<std::uint8_t> scratch);
    Status ReadLogBlock(JournalLog& log, std::uint32_t position, std::span<std::uint8_t> out,
                        std::span<std::uint8_t> scratch);
    // The first transaction the walk did not finish.
    Result<std::uint32_t> WalkLog(JournalLog& log, LogPass pass, Buffers& buffers);
    // Whether a commit block ends the log instead of committing; Corrupt when the transaction
    // must not be replayed and the log must not end before it either.
    Result<bool> CheckCommit(const JournalLog& log, std::uint32_t sequence,
                             const TransactionCheck& check, std::span<const std::uint8_t> block);
    // Walks the blocks a descriptor block (in buffers[0]) tags.
    Status FollowTags(JournalLog& log, LogPass pass, std::uint32_t sequence,
                      TransactionCheck& check, Buffers& buffers);
    Status ClearRecoveryFlag(std::span<std::uint8_t> scratch);

    // Groups and bitmaps (allocation.cpp).
    std::uint32_t BlocksInGroup(std::uint32_t group) const;
    std::uint64_t GroupFirstBlock(std::uint32_t group) const;
    std::uint32_t GroupOfInode(NodeId node) const;
    GroupDescriptor Group(std::uint32_t group) const;
    void StoreGroup(std::uint32_t group, const GroupDescriptor& descriptor);
    bool IsGroupMetadata(std::uint32_t group, std::uint64_t block) const;
    // Why the descriptors cannot be trusted for writing, or nullopt.
    std::optional<std::string_view> CheckGroups() const;
    Status WriteGroups();
    Result<std::uint32_t> AllocateBlock(std::uint32_t goal, std::span<std::uint8_t> bitmap);
    // In the parent's group when it has room, otherwise in the next group that does (Orlov-lite:
    // files stay near their directory).
    // Where a new directory's inode is looked for first (Orlov-lite).
    std::uint32_t DirectoryGroup(std::uint32_t parent_group, bool top_level) const;
    Result<NodeId> AllocateInode(std::uint32_t parent_group, bool directory,
                                 std::span<std::uint8_t> bitmap, std::span<std::uint8_t> scratch);
    Status FreeInode(NodeId node, bool directory, std::span<std::uint8_t> bitmap);
    Status FreeBlock(FreeCursor& cursor, std::uint32_t block);
    Status FinishFrees(FreeCursor& cursor);

    // Changes (write.cpp).
    Timestamp Now() const;
    bool IsReserved(NodeId node) const;
    // Marks the volume not clean on disk before the first change after a mount or Sync.
    Status BeginChange(std::span<std::uint8_t> scratch);
    Status WriteSuperblock(std::span<std::uint8_t> scratch);
    Status MarkMounted();
    // Sets the error state, and stops writes unless s_errors says continue.
    void NoteError(Error error);
    template <typename T> T Noted(T result)
    {
        if (!result)
        {
            NoteError(result.error());
        }
        return result;
    }
    // A fresh inode's slot is zeroed first, so nothing of an earlier inode survives.
    Status StoreInode(NodeId node, const Inode& inode, bool fresh, std::span<std::uint8_t> scratch);
    Status SetLargeFile(std::uint64_t size, std::span<std::uint8_t> scratch);
    Status ClearIndex(NodeId directory, Inode& inode, std::span<std::uint8_t> scratch);
    Status ZeroFrom(const Inode& inode, std::uint64_t position, std::span<std::uint8_t> scratch);
    Result<std::uint32_t> AllocateFor(MapCursor& cursor, std::span<std::uint8_t> bitmap);
    Status ReleaseLevels(MapCursor& cursor, std::uint32_t from);
    // The data block for logical, 0 for a hole, after allocating the missing indirect blocks.
    Result<std::uint32_t> ResolveForWrite(Inode& inode, MapCursor& cursor, std::uint64_t logical,
                                          std::span<std::uint8_t> bitmap);
    // Points logical at block; ResolveForWrite has just walked to it.
    void LinkBlock(Inode& inode, MapCursor& cursor, std::uint64_t logical, std::uint32_t block);
    Result<std::uint64_t> FreeTree(std::uint32_t block, std::uint32_t depth, Buffers& buffers,
                                   FreeCursor& frees);
    Result<std::uint64_t> ShrinkIndirect(std::uint32_t block, std::uint32_t depth,
                                         std::uint64_t first, std::uint64_t keep,
                                         std::uint32_t level, Buffers& buffers, FreeCursor& frees);
    Status ShrinkBlocks(NodeId node, Inode& inode, std::uint64_t size, Buffers& buffers);
    Status ReleaseAttributeBlock(std::uint32_t block, FreeCursor& frees,
                                 std::span<std::uint8_t> scratch);
    // Checks what ReleaseContents will meet, before anything changes.
    Status CheckReleasable(const Inode& inode, std::span<std::uint8_t> scratch);
    Status ReleaseContents(const Node& node, Buffers& buffers);
    Status AddEntry(NodeId directory, Inode& inode, std::optional<RecordPlace> room,
                    const RawEntry& entry, Buffers& buffers);

    Result<std::size_t> WriteFile(NodeId file, std::uint64_t offset,
                                  std::span<const std::uint8_t> data);
    Status TruncateFile(NodeId file, std::uint64_t size);
    Status ChangeAttributes(NodeId node, const AttributeChanges& changes);

    // The directory tree (namespace.cpp).
    Inode NewInode(std::uint16_t mode, Timestamp now) const;
    // Looks for name, and for a record with room for needed bytes when it is not there.
    Result<NameSearch> SearchDirectory(const Inode& directory, std::string_view name,
                                       std::size_t needed, Buffers& buffers);
    // Removes the record at place, which must still name inode.
    Status DropRecord(const RecordPlace& place, std::uint32_t inode, std::span<std::uint8_t> block);
    // Points the record at place, which must still name inode, at replacement.
    Status RewriteRecord(const RecordPlace& place, std::uint32_t inode, std::uint32_t replacement,
                         std::uint8_t file_type, std::span<std::uint8_t> block);
    // Adds links to a directory's link count and, when touched, sets its modification and
    // change times.
    Status UpdateDirectory(NodeId directory, int links, bool touched,
                           std::span<std::uint8_t> scratch);
    // Only ".", ".." and unused records; ".." must name parent.
    Result<bool> IsEmptyDirectory(const Inode& directory, NodeId parent, Buffers& buffers);
    Status CheckOutside(NodeId directory, NodeId moving, Buffers& buffers);
    // One link fewer for a node whose entry is gone; the last frees it, and a directory's last
    // also takes its ".." out of parent's count.
    Status DropLink(NodeId node, const Node& loaded, NodeId parent, Buffers& buffers);
    Result<NodeId> CreateNode(NodeId directory, std::string_view name, std::uint16_t mode,
                              std::string_view target);
    Status RemoveNode(NodeId directory, std::string_view name, bool directory_wanted);
    Status RenameNode(NodeId from_directory, std::string_view from_name, NodeId to_directory,
                      std::string_view to_name);
    Status LinkNode(NodeId node, NodeId directory, std::string_view name);

    BlockDevice& volume;
    Superblock superblock;
    BlockCache cache;
    HostBuffer descriptor_memory;
    std::span<std::uint8_t> descriptors;
    // The descriptor table's blocks that changed since they were last written, [first, end).
    std::uint64_t dirty_descriptors_first = 0;
    std::uint64_t dirty_descriptors_end = 0;
    // Mounted for writing: the superblock is this mount's to write back, even after errors
    // stop further changes.
    bool writable_mount;
    bool read_only;
    // The superblock on disk says clean, so the next change must say otherwise first.
    bool superblock_clean = true;
    bool has_file_type;
    // gdt_csum or metadata_csum: descriptor checksums, flags and unused-inode counts.
    bool group_checksums;
    bool metadata_checksums;
    bool hashed_directories;
    bool extents;
    std::uint32_t volume_seed;
    std::uint32_t descriptor_size;
    std::uint32_t block_size;
    std::uint32_t pointers_per_block;
    std::uint64_t inode_table_blocks;
    std::uint64_t descriptor_blocks;
};

template <typename Visit>
Result<bool> Ext2Filesystem::WalkDirectory(const Inode& directory, std::uint64_t start,
                                           std::span<std::uint8_t> block,
                                           std::span<std::uint8_t> scratch, Visit visit)
{
    if (TypeOf(directory.mode) != NodeType::Directory)
    {
        return std::unexpected(Error::NotDirectory);
    }
    // Directories are whole blocks without holes, so their size cannot exceed what they hold;
    // the check also keeps a corrupt size from sending the walk over billions of blocks.
    if (directory.size % block_size != 0 || directory.size > directory.allocated_bytes)
    {
        return std::unexpected(Error::Corrupt);
    }
    const std::uint64_t blocks = directory.size / block_size;
    BlockRun run = {.logical = 0, .physical = 0, .count = 0};
    for (std::uint64_t logical = start / block_size; logical < blocks; ++logical)
    {
        if (logical - run.logical >= run.count)
        {
            const Result<BlockRun> mapped = MapRun(directory, logical, scratch);
            if (!mapped)
            {
                return Failure(mapped);
            }
            run = *mapped;
        }
        if (run.physical == 0)
        {
            continue;
        }
        const std::uint64_t physical = run.physical + (logical - run.logical);
        if (const Status read = cache.Read(physical, 0, block, block); !read)
        {
            return Failure(read);
        }
        if (metadata_checksums && !DirectoryBlockMatches(directory, logical, block))
        {
            return std::unexpected(Error::Corrupt);
        }
        // Records are walked from the block's start even when resuming, so a cookie that does
        // not fall on one still finds the next.
        std::size_t previous = NoRecord;
        for (std::size_t offset = 0; offset < block_size;)
        {
            const Result<RawEntry> entry =
                ParseEntry(block, offset, has_file_type, superblock.inodes_count);
            if (!entry)
            {
                return Failure(entry);
            }
            const std::uint64_t position = logical * block_size + offset;
            const RecordPlace place = {
                .physical = physical,
                .offset = offset,
                .previous = previous,
                .next = position + entry->record_length,
            };
            previous = offset;
            offset += entry->record_length;
            if (position < start)
            {
                continue;
            }
            const Result<bool> done = visit(*entry, place);
            if (!done || *done)
            {
                return done;
            }
        }
    }
    return false;
}

} // namespace FeatherDisk::Ext

#endif
