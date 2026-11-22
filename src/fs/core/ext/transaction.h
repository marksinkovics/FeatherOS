// SPDX-License-Identifier: MIT OR Apache-2.0
// The jbd2 transactions of a writable ext3 or ext4 volume with an internal journal: the layers of
// block copies a change goes through before its blocks reach their places (transaction.cpp).
#ifndef FEATHERDISK_EXT_TRANSACTION_H
#define FEATHERDISK_EXT_TRANSACTION_H

#include "ext/ext2_filesystem.h"
#include "ext/journal.h"
#include "ext/superblock.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace FeatherDisk::Ext
{

// What the copies of each layer may use of memory, and how long a transaction stays open.
constexpr std::size_t JournalMemoryBytes = 4 * 1024 * 1024;
constexpr std::uint64_t CommitIntervalNanoseconds = 5'000'000'000;

// Block copies by home block number. A dropped copy is replaced by the last one, so the order is
// that of first holding, but for drops. Memory comes from the host.
class BlockLayer
{
public:
    explicit BlockLayer(std::uint32_t block_size);
    ~BlockLayer();
    BlockLayer(const BlockLayer&) = delete;
    BlockLayer& operator=(const BlockLayer&) = delete;

    std::size_t Count() const;
    std::size_t Maximum() const;
    std::uint8_t* Find(std::uint64_t home) const;
    // The existing copy or a new one; null at the maximum or without memory.
    std::uint8_t* Hold(std::uint64_t home);
    void Drop(std::uint64_t home);
    // Room for count copies, or false.
    bool Reserve(std::size_t count);
    // Replaces its copies with from's, which ends empty. Fails without changing either.
    Status Absorb(BlockLayer& from);
    void Clear();
    std::uint64_t HomeAt(std::size_t index) const;
    std::uint8_t* BytesAt(std::size_t index) const;

    template <typename Visit> Status ForEach(Visit visit) const
    {
        for (std::size_t index = 0; index < count; ++index)
        {
            if (const Status visited = visit(HomeAt(index), BytesAt(index)); !visited)
            {
                return visited;
            }
        }
        return {};
    }

private:
    std::size_t Slot(std::uint64_t home) const;
    void Erase(std::size_t at);
    bool Rebuild(std::size_t capacity);

    std::uint32_t block_size;
    std::size_t maximum;
    std::size_t count = 0;
    std::size_t capacity = 0;
    std::uint64_t* homes = nullptr;
    std::uint8_t* bytes = nullptr;
    // Open addressing over homes: an index plus one, 0 for an empty place.
    std::uint32_t* table = nullptr;
    std::size_t table_size = 0;
};

// 64-bit numbers in insertion order, without repeats.
class BlockSet
{
public:
    BlockSet() = default;
    ~BlockSet();
    BlockSet(const BlockSet&) = delete;
    BlockSet& operator=(const BlockSet&) = delete;

    // Room for count values, or false.
    bool Reserve(std::size_t count);
    Status Add(std::uint64_t value);
    bool Contains(std::uint64_t value) const;
    std::size_t Count() const;
    void Clear();
    std::uint64_t At(std::size_t index) const;

    template <typename Visit> Status ForEach(Visit visit) const
    {
        for (std::size_t index = 0; index < count; ++index)
        {
            if (const Status visited = visit(At(index)); !visited)
            {
                return visited;
            }
        }
        return {};
    }

private:
    std::size_t Slot(std::uint64_t value) const;
    bool Rebuild(std::size_t capacity);

    std::size_t count = 0;
    std::size_t capacity = 0;
    std::uint64_t* values = nullptr;
    std::uint32_t* table = nullptr;
    std::size_t table_size = 0;
};

struct Ext2Filesystem::JournalLog
{
    Inode inode;
    JournalSuperblock header;
    std::uint32_t seed;
    // The journal's blocks that the last one read belongs to.
    BlockRun run;
    RevokeTable revokes;
    // The first transaction not replayed.
    std::uint32_t end;
    // The next block of a walk, and how many it may still read: a log is never longer than the
    // ring, and one that seems to be is going round it.
    std::uint32_t position;
    std::uint64_t remaining;

    void Restart();
    Result<std::uint32_t> Advance();
};

struct Ext2Filesystem::JournalState
{
    explicit JournalState(std::uint32_t block_size);
    static void* operator new(std::size_t size, void* place) noexcept;

    // The header holds the features as written.
    JournalLog log;
    // The ring's first and last block and its length.
    std::uint32_t first;
    std::uint32_t end;
    std::uint32_t head;
    // Ring blocks the log holds, from s_start on.
    std::uint32_t used;
    // The next transaction's ID.
    std::uint32_t sequence;
    JournalLimits limits;
    // The change in progress, whole steps not yet committed, and committed transactions the log
    // holds that are not yet written to their places.
    BlockLayer step;
    BlockLayer running;
    BlockLayer committed;
    // Of the running transaction.
    BlockSet revokes;
    BlockSet freed_groups;
    // Of the step.
    BlockSet pending_forgets;
    BlockSet step_freed_groups;
    // Blocks in the step that the running layer does not hold.
    std::size_t step_new;
    Superblock step_superblock;
    bool overflowed;
    // The activity counters as the step began, which a rolled back step does not count.
    std::uint32_t step_orphans_added;
    // The last failed step overflowed with nothing else running.
    bool overflowed_alone;
    bool aborted;
    // The superblock changed outside a step and must be in the next transaction.
    bool superblock_dirty;
    std::optional<std::uint64_t> running_since;
    JournalActivity activity;
};

} // namespace FeatherDisk::Ext

#endif
