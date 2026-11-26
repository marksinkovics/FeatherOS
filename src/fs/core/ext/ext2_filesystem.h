// SPDX-License-Identifier: MIT OR Apache-2.0
// The filesystem behind Ext2Type(), Ext3Type() and Ext4Type(), shared by ext2.cpp (mounting and
// reading), extent.cpp (extent trees), recovery.cpp (journal replay), allocation.cpp (groups and
// bitmaps), write.cpp (changes), extent_write.cpp (changes to extent trees) and namespace.cpp
// (the directory tree).
#ifndef FEATHERDISK_EXT_EXT2_FILESYSTEM_H
#define FEATHERDISK_EXT_EXT2_FILESYSTEM_H

#include "common/block_cache.h"
#include "common/host_buffer.h"
#include "ext/checksum.h"
#include "ext/directory.h"
#include "ext/extent.h"
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
// What ShrinkBlocks takes: three each for the indirect blocks as read and as cut, and the six of a
// change.
constexpr std::size_t ShrinkBuffers = 12;

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
    // count consecutive buffers from index on, as one span.
    std::span<std::uint8_t> Blocks(std::size_t index, std::size_t count);

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
    std::uint64_t logical;
    std::uint64_t physical;
    // In a block of a hash index rather than a leaf.
    bool index;
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

// Blocks given out together by AllocateBlocks: count of them from first on.
struct AllocatedRun
{
    std::uint32_t first;
    std::uint32_t count;
};

constexpr std::size_t NoPosition = ~std::size_t{0};

// One node on the way from an extent tree's root to a leaf, as read.
struct ExtentLevel
{
    // 0 for the root, which lives in i_block.
    std::uint64_t block;
    std::span<std::uint8_t> node;
    std::uint16_t entries;
    // The index entry followed; in the leaf, the last extent starting at or before the logical
    // block looked for, or NoPosition when it lies before them all.
    std::size_t position;
};

// The way from the root to the leaf that maps a logical block, or would map it.
struct ExtentPath
{
    // A copy of i_block, so the root reads as it was while the inode changes.
    std::array<std::uint8_t, BlockArrayBytes> root;
    std::array<ExtentLevel, MaximumExtentDepth + 1> levels;
    // The root's depth: levels[0] is the root, levels[depth] the leaf.
    std::uint16_t depth;
    // The first logical block the leaf may not map: the next index entry's above it.
    std::uint64_t end;
};

// The buffers of an extent tree change, by use: the nodes below the root on a path, two lists of
// entries that may overflow a node (two blocks each), the node being written, and the bitmap of
// the blocks being freed.
constexpr std::size_t ExtentPathBuffers = 0;
constexpr std::size_t ExtentListBuffers = MaximumExtentDepth;
constexpr std::size_t ExtentOutputBuffer = ExtentListBuffers + 4;
constexpr std::size_t ExtentFreeBuffer = ExtentOutputBuffer + 1;
constexpr std::size_t ExtentBuffers = ExtentFreeBuffer + 1;

// One change to a file's extent tree. Tree blocks it allocates and has not linked yet are freed
// again if it fails before its commit, the one write that links them.
struct ExtentEdit
{
    NodeId node;
    Inode* inode;
    Buffers* tree;
    std::span<std::uint8_t> scratch;
    std::span<std::uint8_t> bitmap;
    FreeCursor frees;
    // Where new tree blocks are looked for.
    std::uint32_t goal;
    std::array<std::uint64_t, 2 * (MaximumExtentDepth + 1)> fresh;
    std::size_t fresh_count;
    // Set when a commit write starts and left set when it succeeds: the blocks the edit linked
    // may then be mapped on the disk, so a later failure must not free them.
    bool commit_tried;
};

// The tail of an orphan file block: ob_magic, then ob_checksum, both little-endian.
constexpr std::uint32_t OrphanBlockMagic = 0x0B10CA04;
constexpr std::size_t OrphanTailSize = 8;
std::size_t OrphansPerBlock(std::uint32_t block_size);

// Where an inode is listed as one a crash may leave unfinished: a slot of the orphan file (its
// logical block and the slot in it), or the s_last_orphan list.
enum class OrphanPlace : std::uint8_t
{
    File,
    List,
};

struct OrphanEntry
{
    OrphanPlace place;
    std::uint64_t logical;
    std::size_t slot;
};

// An unlinked inode whose release a split change leaves to ReleaseSplit.
struct OrphanedInode
{
    NodeId node;
    OrphanEntry entry;
};

// In blocks: the footprint a transaction reaches before the next change commits it, the most one
// transaction may hold, and the most the committed layer holds before a checkpoint.
struct JournalLimits
{
    std::uint32_t soft;
    std::uint32_t hard;
    std::uint32_t committed;
};

struct JournalActivity
{
    std::uint32_t transactions;
    std::uint32_t checkpoints;
    std::uint32_t wraps;
    std::uint32_t retries;
    std::uint32_t splits;
    std::uint32_t largest_transaction;
    std::uint32_t orphans_added;
    std::uint32_t orphans_processed;
    std::uint32_t next_sequence;
    std::uint32_t head;
    bool aborted;
};

// Changes (everything from SetAttributes to SymbolicLink, and Sync) must not run at the same time
// as any other operation on the mount; reads may run together. The host serialises them.
class Ext2Filesystem final : public Filesystem
{
public:
    Ext2Filesystem(BlockDevice& device, const Superblock& layout, bool mounted_read_only);
    ~Ext2Filesystem();

    // Constructed in memory from Host::Allocate; the core has no <new> for the global form.
    static void* operator new(std::size_t size, void* place) noexcept;

    Status Initialize();
    // Copies the transactions the journal committed to their places, empties the journal and
    // clears needs_recovery (recovery.cpp). Mount runs it on a read-only instance for a mount
    // asked to write, then mounts the recovered volume.
    Status RecoverJournal();
    // Commits what is running, writes the log home, empties it and clears needs_recovery
    // (transaction.cpp). ExtFilesystemType::Unmount runs it after Sync; the mount stays as it was
    // when it fails.
    Status CloseJournal();
    // What the journal did so far, for tests: nullopt without one.
    std::optional<JournalActivity> Activity() const;
    // Only lowers the limits, for tests.
    void LimitJournal(const JournalLimits& limits);
    // Blocks the metadata cache holds copies of at most, 0 when the device caches them itself;
    // for tests.
    std::size_t MetadataCacheSlots() const;

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
    Status MakeReadOnly() override;

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
    // Of an extent-mapped file, or of a block-mapped one.
    std::uint64_t MaximumFileSize(bool extent_mapped) const;
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
    // Reads the child an index entry names into node and checks it: one level below
    // parent_depth, an eh_max a block holds and, with metadata_csum, its tail.
    Result<ExtentHeader> LoadExtentNode(const Inode& inode, std::uint64_t child,
                                        std::uint16_t parent_depth, std::span<std::uint8_t> node);
    // The block holding a file's logical block, or 0 for a hole or an unwritten extent.
    Result<std::uint64_t> PhysicalBlock(const Inode& inode, std::uint64_t logical,
                                        std::span<std::uint8_t> scratch);
    // Calls visit(entry, place) for each record from byte start on, unused ones (inode 0)
    // included, until it returns true. block receives each directory block and still holds the
    // block of the record visit stopped at; scratch serves the rest.
    template <typename Visit>
    Result<bool> WalkDirectory(const Inode& directory, std::uint64_t start,
                               std::span<std::uint8_t> block, std::span<std::uint8_t> scratch,
                               Visit visit);
    // A block of an indexed directory that holds index entries instead of names.
    bool IsIndexOf(const Inode& directory, std::uint64_t logical,
                   std::span<const std::uint8_t> block) const;
    bool DirectoryBlockMatches(const Inode& directory, std::uint64_t logical,
                               std::span<const std::uint8_t> block) const;
    Result<NodeType> EntryType(const RawEntry& entry, std::span<std::uint8_t> scratch);
    Result<std::size_t> ReadContents(const Inode& file, std::uint64_t offset,
                                     std::span<std::uint8_t> buffer,
                                     std::span<std::uint8_t> scratch);

    // Journal replay (recovery.cpp).
    struct JournalLog;
    struct JournalState;
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
    // Without report, a journal that cannot be used is not logged.
    Status OpenJournal(JournalLog& log, Buffers& buffers, bool report = true);
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
    void MarkGroupMetadata(std::uint32_t group, std::span<std::uint8_t> bitmap) const;
    void SetPadding(std::span<std::uint8_t> bitmap, std::uint32_t used_bits) const;
    std::uint32_t BitmapChecksumOf(std::span<const std::uint8_t> bitmap, std::uint32_t bits) const;
    // Logs, stops writes and gives Corrupt.
    std::unexpected<Error> BitmapMismatch(std::string_view message, std::uint32_t group);
    Status LoadBlockBitmap(std::uint32_t group, std::span<std::uint8_t> bitmap);
    Status LoadInodeBitmap(std::uint32_t group, std::span<std::uint8_t> bitmap);
    // Writes a changed bitmap, then keeps descriptor as the group's.
    Status CommitBitmap(std::uint32_t group, GroupDescriptor descriptor, bool blocks,
                        std::span<const std::uint8_t> bitmap);
    // Why the descriptors cannot be trusted for writing, or nullopt.
    std::optional<std::string_view> CheckGroups() const;
    Status WriteGroups();
    Result<std::uint32_t> AllocateBlock(std::uint32_t goal, std::span<std::uint8_t> bitmap);
    // Up to wanted consecutive blocks, the first at or after goal as AllocateBlock finds it.
    Result<AllocatedRun> AllocateBlocks(std::uint32_t goal, std::uint32_t wanted,
                                        std::span<std::uint8_t> bitmap);
    // In the parent's group when it has room, otherwise in the next group that does (Orlov-lite:
    // files stay near their directory).
    // Where a new directory's inode is looked for first (Orlov-lite).
    std::uint32_t DirectoryGroup(std::uint32_t parent_group, bool top_level) const;
    Result<NodeId> AllocateInode(std::uint32_t parent_group, bool directory,
                                 std::span<std::uint8_t> bitmap, std::span<std::uint8_t> scratch);
    Status FreeInode(NodeId node, bool directory, std::span<std::uint8_t> bitmap);
    Status FreeBlock(FreeCursor& cursor, std::uint64_t block);
    Status FreeBlocks(FreeCursor& cursor, std::uint64_t first, std::uint64_t count);
    Status FinishFrees(FreeCursor& cursor);

    // Block access and transactions (transaction.cpp): metadata goes through the journal's layers
    // when there is one, data in place.
    Status ReadMetadata(std::uint64_t block, std::size_t offset, std::span<std::uint8_t> out,
                        std::span<std::uint8_t> scratch);
    Status WriteMetadata(std::uint64_t first_block, std::span<const std::uint8_t> bytes);
    Status WriteData(std::uint64_t first_block, std::span<const std::uint8_t> bytes);
    // Opens the journal of a writable mount before MarkMounted; a volume whose journal cannot be
    // written mounts read-only, with a log line.
    Status StartJournal();
    void DestroyJournal();
    // The committed layer, then the device.
    Status ReadCommitted(std::uint64_t block, std::span<std::uint8_t> out);
    // The ring blocks a transaction with that many copies and revokes takes.
    std::uint32_t Footprint(std::size_t blocks, std::size_t revokes) const;
    Status BeginStep();
    Status EndStep();
    void AbortStep();
    // The step that just failed overflowed the transaction with nothing else running in it, so
    // the change may be split into several.
    bool OverflowedAlone() const;
    // Rolls a failed step back. retry is set when the change should run again, now alone;
    // otherwise the result is the error to report.
    Error FailStep(Error error, bool may_retry, bool& retry);
    bool CommitDue() const;
    Status NoteFreedBlock(std::uint32_t group, std::uint64_t block);
    // A group's block bitmap as the committed state has it, which a block freed since stays in
    // use in until the freeing commits.
    Status LoadCommittedBlockBitmap(std::uint32_t group, std::span<std::uint8_t> bitmap,
                                    std::span<std::uint8_t> scratch);
    bool GroupFrozen(std::uint32_t group) const;
    Status CommitTransaction();
    Status Checkpoint();
    Status WriteJournalPosition(std::uint32_t start, std::uint32_t sequence, Buffers& buffers);
    Result<std::uint64_t> NextLogPlace(std::span<std::uint8_t> scratch);
    Status WriteRevokeBlocks(Buffers& buffers);
    Status WriteDescriptorGroups(Buffers& buffers);
    void AbortJournal(std::string_view why);
    // Runs change as one step of a transaction (or plainly on a volume without a journal).
    template <typename Change> auto Journalled(Change change) -> decltype(change());

    // Orphans and split changes (orphan.cpp).
    bool HasOrphanFile() const;
    void NoteSplit();
    // The orphan file's inode, checked to be a regular file of whole blocks.
    Result<Inode> LoadOrphanFile(std::span<std::uint8_t> scratch);
    // Its logical block's place on the volume; Corrupt for a hole.
    Result<std::uint64_t> OrphanBlockPlace(const Inode& file, std::uint64_t logical,
                                           std::span<std::uint8_t> scratch);
    bool OrphanBlockMatches(const Inode& file, std::uint64_t place,
                            std::span<const std::uint8_t> block) const;
    void StoreOrphanTail(const Inode& file, std::uint64_t place,
                         std::span<std::uint8_t> block) const;
    Result<OrphanEntry> AddOrphan(NodeId node, Inode& inode);
    Status RemoveOrphan(NodeId node, Inode& inode, const OrphanEntry& entry);
    // Clears orphan_present when no slot holds an inode.
    Status UpdateOrphanPresent();
    Status CheckOrphans(Buffers& buffers);
    Status ProcessOrphans();
    Status ProcessOrphan(NodeId node, const OrphanEntry& entry);
    // The end of what an inode maps: no block at or past it is mapped.
    Result<std::uint64_t> MappedEnd(const Inode& inode);
    Status ShrinkTo(NodeId node, Inode& inode, std::uint64_t keep, Buffers& buffers);
    // Frees the blocks from keep on in steps of ever fewer blocks; the step that reaches keep
    // also removes entry, when there is one.
    Status ShrinkSplit(NodeId node, std::uint64_t keep, const OrphanEntry* entry);
    Status ShrinkPiece(NodeId node, std::uint64_t keep, const OrphanEntry* entry);
    Status ReleaseSplit(NodeId node, const OrphanEntry& entry);
    Status FinishRelease(NodeId node, const OrphanEntry& entry);
    Status TruncateSplit(NodeId file, std::uint64_t size);
    Result<std::size_t> WriteSplit(NodeId file, std::uint64_t offset,
                                   std::span<const std::uint8_t> data);
    Status FinishWriteSplit(NodeId file, std::uint64_t size, const OrphanEntry& entry);

    // Changes (write.cpp).
    Timestamp Now() const;
    bool IsReserved(NodeId node) const;
    // Marks the volume not clean on disk before the first change after a mount or Sync.
    Status BeginChange(std::span<std::uint8_t> scratch);
    Status WriteSuperblock(std::span<std::uint8_t> scratch);
    // The superblock's block with the state in memory, from the layers or, direct, the device.
    Status EncodeSuperblockBlock(std::span<std::uint8_t> block, bool direct);
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
    // Writes a changed directory block with its checksum, if the volume keeps them.
    Status WriteDirectoryBlock(const Inode& directory, std::uint64_t logical,
                               std::uint64_t physical, std::span<std::uint8_t> block);
    Status ClearIndex(NodeId directory, Inode& inode, Buffers& buffers);
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
    // size_stored: the inode already holds the new size, and the last block is already zeroed.
    Status ShrinkBlocks(NodeId node, Inode& inode, std::uint64_t size, bool size_stored,
                        Buffers& buffers);
    Status ReleaseAttributeBlock(std::uint32_t block, FreeCursor& frees,
                                 std::span<std::uint8_t> scratch);
    // The header Linux requires (magic, one block) and, with metadata_csum, the checksum.
    bool AttributeBlockMatches(std::uint32_t block, std::span<const std::uint8_t> contents) const;
    // Checks what ReleaseContents will meet, before anything changes.
    Status CheckReleasable(const Inode& inode, std::span<std::uint8_t> scratch);
    Status ReleaseContents(const Node& node, Buffers& buffers);
    Status AddEntry(NodeId directory, Inode& inode, std::optional<RecordPlace> room,
                    const RawEntry& entry, Buffers& buffers);

    // Extent trees (extent_write.cpp).
    ExtentEdit BeginExtentEdit(NodeId node, Inode& inode, Buffers& tree, Buffers& buffers);
    Status FindExtentPath(const Inode& inode, std::uint64_t logical, ExtentPath& path,
                          Buffers& tree);
    Result<std::uint64_t> AllocateTreeBlock(ExtentEdit& edit);
    void ForgetFresh(ExtentEdit& edit);
    Status WriteTreeNode(ExtentEdit& edit, std::uint64_t block, std::span<const std::uint8_t> list,
                         std::size_t first, std::size_t count, std::uint16_t depth);
    // Gives the node at level of path the entries list holds (laid out as a node, and possibly
    // more than a node holds): in place, or in new blocks linked from the level above.
    Status ReplaceExtentNode(ExtentEdit& edit, const ExtentPath& path, std::size_t level,
                             std::span<std::uint8_t> list, std::size_t count);
    // Maps a hole that path leads to.
    Status InsertExtent(ExtentEdit& edit, const ExtentPath& path, const Extent& extent);
    // Turns count blocks from logical on, inside the unwritten extent path's leaf points to,
    // into written ones.
    Status MarkWritten(ExtentEdit& edit, const ExtentPath& path, std::uint64_t logical,
                       std::uint64_t count);
    Status ShrinkExtents(NodeId node, Inode& inode, std::uint64_t keep, Buffers& buffers);
    Status FreeExtentSubtree(const Inode& inode, std::span<const std::uint8_t> node,
                             std::uint16_t depth, Buffers& tree, FreeCursor& frees);
    Status ReleaseExtents(const Inode& inode, FreeCursor& frees);
    // One past the last logical block an extent-mapped file maps; 0 for none.
    Result<std::uint64_t> ExtentMappedEnd(const Inode& inode);
    // Writes count blocks of a file's data from logical on to blocks from physical on, taking
    // the bytes of data that fall in them; the other bytes are read first, or zeros when fresh.
    Status WriteDataBlocks(std::uint64_t physical, std::uint64_t logical, std::uint64_t count,
                           std::uint64_t offset, std::span<const std::uint8_t> data, bool fresh,
                           std::span<std::uint8_t> block);
    // written counts the bytes that reached the file, also when a later part fails.
    Status WriteExtentFile(NodeId file, Inode& inode, std::uint64_t offset,
                           std::span<const std::uint8_t> data, Buffers& buffers,
                           std::size_t& written);
    // A directory block at the end of an extent-mapped directory, written before it is mapped.
    Status AppendExtentBlock(NodeId directory, Inode& inode, std::span<std::uint8_t> contents,
                             Buffers& buffers);

    // The size and times after written bytes, and the inode with them.
    // keep_size: the size stays as it is, for a piece of a split write whose orphan entry stands in
    // for it.
    Result<std::size_t> FinishWrite(NodeId file, Inode& inode, std::uint64_t offset,
                                    std::size_t written, Status failure, bool keep_size,
                                    std::span<std::uint8_t> scratch);
    // zeroed: what lay between the old end and the write is zeroed already, by an earlier piece.
    Result<std::size_t> WriteFile(NodeId file, std::uint64_t offset,
                                  std::span<const std::uint8_t> data, bool keep_size = false,
                                  bool zeroed = false);
    Status TruncateFile(NodeId file, std::uint64_t size);
    Status ChangeAttributes(NodeId node, const AttributeChanges& changes);

    // The directory tree (namespace.cpp).
    Inode NewInode(std::uint16_t mode, Timestamp now) const;
    // Looks for name, and for a record with room for needed bytes when it is not there.
    Result<NameSearch> SearchDirectory(const Inode& directory, std::string_view name,
                                       std::size_t needed, Buffers& buffers);
    // Removes the record at place in directory, which must still name inode.
    Status DropRecord(const Inode& directory, const RecordPlace& place, std::uint32_t inode,
                      std::span<std::uint8_t> block);
    // Points the record at place, which must still name inode, at replacement.
    Status RewriteRecord(const Inode& directory, const RecordPlace& place, std::uint32_t inode,
                         std::uint32_t replacement, std::uint8_t file_type,
                         std::span<std::uint8_t> block);
    // The most links an inode may have.
    std::uint16_t LinkLimit() const;
    // A directory's link count with delta more subdirectories, or nullopt when that is damage:
    // with dir_nlink, a count past the limit becomes 1 and stays 1, as on Linux.
    std::optional<std::uint16_t> DirectoryLinks(std::uint16_t count, int delta) const;
    // Whether a directory's count holds at least one subdirectory's "..".
    bool HasSubdirectoryLink(std::uint16_t count) const;
    // Adds links to a directory's link count and, when touched, sets its modification and
    // change times.
    Status UpdateDirectory(NodeId directory, int links, bool touched,
                           std::span<std::uint8_t> scratch);
    // Only ".", ".." and unused records; ".." must name parent.
    Result<bool> IsEmptyDirectory(const Inode& directory, NodeId parent, Buffers& buffers);
    Status CheckOutside(NodeId directory, NodeId moving, Buffers& buffers);
    // One link fewer for a node whose entry is gone; the last frees it, and a directory's last
    // also takes its ".." out of parent's count.
    // With orphaned, the last link of a file or link takes it to the orphan list instead of
    // releasing it in this step.
    Status DropLink(NodeId node, const Node& loaded, NodeId parent, Buffers& buffers,
                    std::optional<OrphanedInode>* orphaned = nullptr);
    Result<NodeId> CreateNode(NodeId directory, std::string_view name, std::uint16_t mode,
                              std::string_view target);
    Status RemoveNode(NodeId directory, std::string_view name, bool directory_wanted,
                      std::optional<OrphanedInode>* orphaned = nullptr);
    Status RenameNode(NodeId from_directory, std::string_view from_name, NodeId to_directory,
                      std::string_view to_name, std::optional<OrphanedInode>* orphaned = nullptr);
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
    // dir_nlink: a directory past the link limit keeps a count of 1.
    bool directory_links;
    std::uint32_t volume_seed;
    std::uint32_t descriptor_size;
    std::uint32_t block_size;
    std::uint32_t pointers_per_block;
    std::uint64_t inode_table_blocks;
    std::uint64_t descriptor_blocks;
    // Non-null only on a writable mount with a usable internal journal.
    JournalState* journal = nullptr;
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
        if (const Status read = ReadMetadata(physical, 0, block, block); !read)
        {
            return Failure(read);
        }
        if (metadata_checksums && !DirectoryBlockMatches(directory, logical, block))
        {
            return std::unexpected(Error::Corrupt);
        }
        // A leaf's checksum record is not an entry.
        const bool index = IsIndexOf(directory, logical, block);
        const std::size_t end =
            metadata_checksums && !index ? block_size - DirectoryTailSize : block_size;
        // Records are walked from the block's start even when resuming, so a cookie that does
        // not fall on one still finds the next.
        std::size_t previous = NoRecord;
        for (std::size_t offset = 0; offset < end;)
        {
            const Result<RawEntry> entry =
                ParseEntry(block.first(end), offset, has_file_type, superblock.inodes_count);
            if (!entry)
            {
                return Failure(entry);
            }
            const std::uint64_t position = logical * block_size + offset;
            const RecordPlace place = {
                .logical = logical,
                .physical = physical,
                .index = index,
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

template <typename Change> auto Ext2Filesystem::Journalled(Change change) -> decltype(change())
{
    if (read_only)
    {
        return Refused();
    }
    if (journal == nullptr)
    {
        return Noted(change());
    }
    for (bool may_retry = true;; may_retry = false)
    {
        if (const Status begun = BeginStep(); !begun)
        {
            return std::unexpected(begun.error());
        }
        auto result = change();
        std::optional<Error> failure;
        if (!result)
        {
            failure = result.error();
        }
        else if (const Status ended = EndStep(); !ended)
        {
            failure = ended.error();
        }
        if (!failure)
        {
            if (CommitDue())
            {
                if (const Status committed = CommitTransaction(); !committed)
                {
                    return std::unexpected(committed.error());
                }
            }
            return result;
        }
        bool retry = false;
        const Error reported = FailStep(*failure, may_retry, retry);
        if (!retry)
        {
            return std::unexpected(reported);
        }
    }
}

} // namespace FeatherDisk::Ext

#endif
