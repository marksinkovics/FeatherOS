// SPDX-License-Identifier: MIT OR Apache-2.0
// Where blocks go, and the jbd2 transactions of a writable ext3 or ext4 volume. Metadata is told
// apart from file data at every access: file data goes to its place at once (ordered mode), while
// every metadata write goes into one of three layers of block copies, and reads look at them
// first:
//   - step: the change in progress, which either joins the running transaction whole or leaves no
//     trace;
//   - running: the steps not yet committed, which make up the next transaction;
//   - committed: the transactions in the log that are not yet written to their places.
// A commit writes the running copies to the log, flushes, writes the commit block and flushes
// again, so a replay sees a transaction whole or not at all; a checkpoint writes the committed
// copies to their places and then empties the log. The order of every write is the one the
// design (docs/design/M22-P6-s4.md, section 5) names C0 to C9 and K1 to K4. A block freed while a
// transaction is running leaves the layers (and, when the log holds a copy of it, gets a revoke
// record), and is not handed out again before the freeing transaction has committed, so a replay
// can never put an old copy of it over what its next owner wrote.
#include "common/bytes.h"
#include "common/host_buffer.h"
#include "ext/ext2_filesystem.h"
#include "ext/journal.h"
#include "ext/transaction.h"
#include "fs/host.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace FeatherDisk::Ext
{
namespace
{

constexpr std::size_t FirstCapacity = 16;

std::size_t HashOf(std::uint64_t value)
{
    constexpr std::uint64_t Golden = 0x9E3779B97F4A7C15;
    return static_cast<std::size_t>((value * Golden) >> 32);
}

// A table of at least twice as many places as entries, a power of two.
std::size_t TableSizeFor(std::size_t capacity)
{
    std::size_t size = 2;
    while (size < capacity * 2)
    {
        size *= 2;
    }
    return size;
}

void ClearTable(std::uint32_t* table, std::size_t size)
{
    for (std::size_t index = 0; index < size; ++index)
    {
        table[index] = 0;
    }
}

// Removes the entry at place by shifting back what probed past it, so lookups never need a mark.
template <typename HomeOf>
void EraseFromTable(std::uint32_t* table, std::size_t size, std::size_t place, HomeOf home_of)
{
    const std::size_t mask = size - 1;
    std::size_t hole = place;
    table[hole] = 0;
    for (std::size_t next = (hole + 1) & mask; table[next] != 0; next = (next + 1) & mask)
    {
        const std::size_t ideal = HashOf(home_of(table[next] - 1)) & mask;
        const bool between =
            hole <= next ? (ideal > hole && ideal <= next) : (ideal > hole || ideal <= next);
        if (!between)
        {
            table[hole] = table[next];
            table[next] = 0;
            hole = next;
        }
    }
}

struct Allocation
{
    explicit Allocation(std::size_t size) : memory(size == 0 ? nullptr : Host::Allocate(size)) {}
    ~Allocation()
    {
        Host::Free(memory);
    }
    Allocation(const Allocation&) = delete;
    Allocation& operator=(const Allocation&) = delete;

    void* memory;
};

void SiftDown(const BlockLayer& layer, std::uint32_t* order, std::size_t root, std::size_t end)
{
    while (2 * root + 1 < end)
    {
        std::size_t child = 2 * root + 1;
        if (child + 1 < end && layer.HomeAt(order[child]) < layer.HomeAt(order[child + 1]))
        {
            ++child;
        }
        if (layer.HomeAt(order[root]) >= layer.HomeAt(order[child]))
        {
            return;
        }
        std::swap(order[root], order[child]);
        root = child;
    }
}

void SortByHome(const BlockLayer& layer, std::uint32_t* order, std::size_t count)
{
    for (std::size_t index = 0; index < count; ++index)
    {
        order[index] = static_cast<std::uint32_t>(index);
    }
    for (std::size_t start = count / 2; start-- > 0;)
    {
        SiftDown(layer, order, start, count);
    }
    for (std::size_t end = count; end-- > 1;)
    {
        std::swap(order[0], order[end]);
        SiftDown(layer, order, 0, end);
    }
}

} // namespace

BlockLayer::BlockLayer(std::uint32_t block_size)
    : block_size(block_size), maximum(JournalMemoryBytes / block_size)
{
}

BlockLayer::~BlockLayer()
{
    Host::Free(homes);
    Host::Free(bytes);
    Host::Free(table);
}

std::size_t BlockLayer::Count() const
{
    return count;
}

std::size_t BlockLayer::Maximum() const
{
    return maximum;
}

std::size_t BlockLayer::Slot(std::uint64_t home) const
{
    const std::size_t mask = table_size - 1;
    std::size_t place = HashOf(home) & mask;
    while (table[place] != 0 && homes[table[place] - 1] != home)
    {
        place = (place + 1) & mask;
    }
    return place;
}

std::uint8_t* BlockLayer::Find(std::uint64_t home) const
{
    if (count == 0 || bytes == nullptr)
    {
        return nullptr;
    }
    const std::size_t place = Slot(home);
    return table[place] == 0 ? nullptr : bytes + std::size_t{table[place] - 1} * block_size;
}

bool BlockLayer::Rebuild(std::size_t new_capacity)
{
    const std::size_t new_table_size = TableSizeFor(new_capacity);
    auto* new_homes =
        static_cast<std::uint64_t*>(Host::Allocate(new_capacity * sizeof(std::uint64_t)));
    auto* new_bytes = static_cast<std::uint8_t*>(Host::Allocate(new_capacity * block_size));
    auto* new_table =
        static_cast<std::uint32_t*>(Host::Allocate(new_table_size * sizeof(std::uint32_t)));
    if (new_homes == nullptr || new_bytes == nullptr || new_table == nullptr)
    {
        Host::Free(new_homes);
        Host::Free(new_bytes);
        Host::Free(new_table);
        return false;
    }
    for (std::size_t index = 0; index < count; ++index)
    {
        new_homes[index] = homes[index];
    }
    if (count != 0)
    {
        CopyBytes(new_bytes, bytes, count * block_size);
    }
    Host::Free(homes);
    Host::Free(bytes);
    Host::Free(table);
    homes = new_homes;
    bytes = new_bytes;
    table = new_table;
    table_size = new_table_size;
    capacity = new_capacity;
    ClearTable(table, table_size);
    for (std::size_t index = 0; index < count; ++index)
    {
        table[Slot(homes[index])] = static_cast<std::uint32_t>(index + 1);
    }
    return true;
}

bool BlockLayer::Reserve(std::size_t wanted)
{
    if (wanted > maximum)
    {
        return false;
    }
    if (wanted <= capacity)
    {
        return true;
    }
    std::size_t grown = capacity == 0 ? FirstCapacity : capacity * 2;
    while (grown < wanted)
    {
        grown *= 2;
    }
    return Rebuild(grown < maximum ? grown : maximum);
}

std::uint8_t* BlockLayer::Hold(std::uint64_t home)
{
    if (std::uint8_t* existing = Find(home))
    {
        return existing;
    }
    if ((count == capacity && !Reserve(count + 1)) || bytes == nullptr)
    {
        return nullptr;
    }
    homes[count] = home;
    table[Slot(home)] = static_cast<std::uint32_t>(count + 1);
    ++count;
    return bytes + (count - 1) * block_size;
}

void BlockLayer::Drop(std::uint64_t home)
{
    if (count == 0)
    {
        return;
    }
    const std::size_t place = Slot(home);
    if (table[place] == 0)
    {
        return;
    }
    const std::size_t slot = table[place] - 1;
    EraseFromTable(table, table_size, place, [this](std::size_t index) { return homes[index]; });
    const std::size_t last = count - 1;
    if (slot != last)
    {
        table[Slot(homes[last])] = static_cast<std::uint32_t>(slot + 1);
        homes[slot] = homes[last];
        CopyBytes(bytes + slot * block_size, bytes + last * block_size, block_size);
    }
    --count;
}

Status BlockLayer::Absorb(BlockLayer& from)
{
    std::size_t fresh = 0;
    for (std::size_t index = 0; index < from.count; ++index)
    {
        fresh += Find(from.homes[index]) == nullptr ? 1 : 0;
    }
    if (!Reserve(count + fresh))
    {
        return std::unexpected(Error::OutOfMemory);
    }
    for (std::size_t index = 0; index < from.count; ++index)
    {
        std::uint8_t* copy = Hold(from.homes[index]);
        CopyBytes(copy, from.bytes + index * block_size, block_size);
    }
    from.Clear();
    return {};
}

void BlockLayer::Clear()
{
    count = 0;
    if (table != nullptr)
    {
        ClearTable(table, table_size);
    }
}

std::uint64_t BlockLayer::HomeAt(std::size_t index) const
{
    return homes[index];
}

std::uint8_t* BlockLayer::BytesAt(std::size_t index) const
{
    return bytes + index * block_size;
}

BlockSet::~BlockSet()
{
    Host::Free(values);
    Host::Free(table);
}

std::size_t BlockSet::Slot(std::uint64_t value) const
{
    const std::size_t mask = table_size - 1;
    std::size_t place = HashOf(value) & mask;
    while (table[place] != 0 && values[table[place] - 1] != value)
    {
        place = (place + 1) & mask;
    }
    return place;
}

bool BlockSet::Rebuild(std::size_t new_capacity)
{
    const std::size_t new_table_size = TableSizeFor(new_capacity);
    auto* new_values =
        static_cast<std::uint64_t*>(Host::Allocate(new_capacity * sizeof(std::uint64_t)));
    auto* new_table =
        static_cast<std::uint32_t*>(Host::Allocate(new_table_size * sizeof(std::uint32_t)));
    if (new_values == nullptr || new_table == nullptr)
    {
        Host::Free(new_values);
        Host::Free(new_table);
        return false;
    }
    for (std::size_t index = 0; index < count; ++index)
    {
        new_values[index] = values[index];
    }
    Host::Free(values);
    Host::Free(table);
    values = new_values;
    table = new_table;
    table_size = new_table_size;
    capacity = new_capacity;
    ClearTable(table, table_size);
    for (std::size_t index = 0; index < count; ++index)
    {
        table[Slot(values[index])] = static_cast<std::uint32_t>(index + 1);
    }
    return true;
}

bool BlockSet::Reserve(std::size_t wanted)
{
    if (wanted <= capacity)
    {
        return true;
    }
    std::size_t grown = capacity == 0 ? FirstCapacity : capacity * 2;
    while (grown < wanted)
    {
        grown *= 2;
    }
    return Rebuild(grown);
}

Status BlockSet::Add(std::uint64_t value)
{
    if (Contains(value))
    {
        return {};
    }
    if (!Reserve(count + 1))
    {
        return std::unexpected(Error::OutOfMemory);
    }
    values[count] = value;
    table[Slot(value)] = static_cast<std::uint32_t>(count + 1);
    ++count;
    return {};
}

bool BlockSet::Contains(std::uint64_t value) const
{
    return count != 0 && table[Slot(value)] != 0;
}

std::size_t BlockSet::Count() const
{
    return count;
}

void BlockSet::Clear()
{
    count = 0;
    if (table != nullptr)
    {
        ClearTable(table, table_size);
    }
}

std::uint64_t BlockSet::At(std::size_t index) const
{
    return values[index];
}

Ext2Filesystem::JournalState::JournalState(std::uint32_t block_size)
    : log{}, first(0), end(0), head(0), used(0), sequence(0), limits{}, step(block_size),
      running(block_size), committed(block_size), step_new(0), step_superblock{}, overflowed(false),
      step_orphans_added(0), overflowed_alone(false), aborted(false), superblock_dirty(false),
      activity{}
{
}

void* Ext2Filesystem::JournalState::operator new(std::size_t /*size*/, void* place) noexcept
{
    return place;
}

Ext2Filesystem::~Ext2Filesystem()
{
    DestroyJournal();
}

void Ext2Filesystem::DestroyJournal()
{
    if (journal != nullptr)
    {
        journal->~JournalState();
        Host::Free(journal);
        journal = nullptr;
    }
}

std::optional<JournalActivity> Ext2Filesystem::Activity() const
{
    if (journal == nullptr)
    {
        return std::nullopt;
    }
    JournalActivity activity = journal->activity;
    activity.next_sequence = journal->sequence;
    activity.head = journal->head;
    activity.aborted = journal->aborted;
    return activity;
}

void Ext2Filesystem::LimitJournal(const JournalLimits& limits)
{
    if (journal == nullptr)
    {
        return;
    }
    JournalLimits& current = journal->limits;
    current.soft = limits.soft < current.soft ? limits.soft : current.soft;
    current.hard = limits.hard < current.hard ? limits.hard : current.hard;
    current.committed = limits.committed < current.committed ? limits.committed : current.committed;
}

Status Ext2Filesystem::ReadMetadata(std::uint64_t block, std::size_t offset,
                                    std::span<std::uint8_t> out, std::span<std::uint8_t> scratch)
{
    if (journal != nullptr)
    {
        const std::array<const BlockLayer*, 3> layers = {&journal->step, &journal->running,
                                                         &journal->committed};
        for (const BlockLayer* layer : layers)
        {
            if (const std::uint8_t* copy = layer->Find(block))
            {
                CopyBytes(out.data(), copy + offset, out.size());
                return {};
            }
        }
    }
    return cache.Read(block, offset, out, scratch);
}

Status Ext2Filesystem::ReadCommitted(std::uint64_t block, std::span<std::uint8_t> out)
{
    if (journal != nullptr)
    {
        if (const std::uint8_t* copy = journal->committed.Find(block))
        {
            CopyBytes(out.data(), copy, block_size);
            return {};
        }
    }
    return cache.Read(block, 0, out, out);
}

Status Ext2Filesystem::WriteData(std::uint64_t first_block, std::span<const std::uint8_t> bytes)
{
    return cache.Write(first_block, bytes);
}

std::uint32_t Ext2Filesystem::Footprint(std::size_t blocks, std::size_t revokes) const
{
    const std::uint32_t incompatible = journal->log.header.incompatible;
    const std::size_t tags = JournalTagsPerDescriptor(block_size, incompatible);
    const std::size_t per_revoke_block = JournalRevokesPerBlock(block_size, incompatible);
    return static_cast<std::uint32_t>(DivideRoundingUp(blocks, tags) + blocks +
                                      DivideRoundingUp(revokes, per_revoke_block) + 1);
}

Status Ext2Filesystem::WriteMetadata(std::uint64_t first_block, std::span<const std::uint8_t> bytes)
{
    if (journal == nullptr)
    {
        return cache.Write(first_block, bytes);
    }
    JournalState& state = *journal;
    const std::uint64_t superblock_block = SuperblockOffset / block_size;
    const std::size_t blocks = bytes.size() / block_size;
    for (std::size_t index = 0; index < blocks; ++index)
    {
        const std::uint64_t home = first_block + index;
        std::uint8_t* copy = state.step.Find(home);
        if (copy == nullptr)
        {
            const bool in_running = state.running.Find(home) != nullptr;
            copy = state.step.Hold(home);
            if (copy == nullptr)
            {
                return std::unexpected(Error::OutOfMemory);
            }
            state.step_new += in_running ? 0 : 1;
            // The superblock's block is in every transaction, so it counts from the start.
            const bool superblock_counted = state.running.Find(superblock_block) != nullptr ||
                                            state.step.Find(superblock_block) != nullptr;
            const std::size_t distinct =
                state.running.Count() + state.step_new + (superblock_counted ? 0 : 1);
            if (distinct > state.limits.hard ||
                Footprint(distinct, state.revokes.Count() + state.pending_forgets.Count()) >
                    state.end - state.first - 1)
            {
                state.step.Drop(home);
                state.step_new -= in_running ? 0 : 1;
                state.overflowed = true;
                return std::unexpected(Error::NoSpace);
            }
        }
        CopyBytes(copy, bytes.data() + index * block_size, block_size);
    }
    return {};
}

bool Ext2Filesystem::GroupFrozen(std::uint32_t group) const
{
    return journal != nullptr &&
           (journal->freed_groups.Contains(group) || journal->step_freed_groups.Contains(group));
}

Status Ext2Filesystem::NoteFreedBlock(std::uint32_t group, std::uint64_t block)
{
    if (journal == nullptr)
    {
        return {};
    }
    JournalState& state = *journal;
    const bool in_running = state.running.Find(block) != nullptr;
    if (state.step.Find(block) != nullptr)
    {
        state.step_new -= in_running ? 0 : 1;
        state.step.Drop(block);
    }
    if (in_running || state.committed.Find(block) != nullptr)
    {
        if (const Status added = state.pending_forgets.Add(block); !added)
        {
            return added;
        }
    }
    return state.step_freed_groups.Add(group);
}

Status Ext2Filesystem::LoadCommittedBlockBitmap(std::uint32_t group, std::span<std::uint8_t> bitmap,
                                                std::span<std::uint8_t> scratch)
{
    const std::size_t offset = std::size_t{group} * descriptor_size;
    if (const Status read =
            ReadCommitted(superblock.first_data_block + 1 + offset / block_size, scratch);
        !read)
    {
        return read;
    }
    const GroupDescriptor descriptor =
        DecodeGroupDescriptor(scratch.subspan(offset % block_size, descriptor_size));
    if (group_checksums && (descriptor.flags & GroupBlocksUnused) != 0)
    {
        ZeroBytes(bitmap);
        MarkGroupMetadata(group, bitmap);
        SetPadding(bitmap, BlocksInGroup(group));
        return {};
    }
    return ReadCommitted(descriptor.block_bitmap, bitmap);
}

bool Ext2Filesystem::CommitDue() const
{
    const JournalState& state = *journal;
    if (state.aborted)
    {
        return false;
    }
    if (state.running.Count() == 0 && state.revokes.Count() == 0 && !state.superblock_dirty)
    {
        return false;
    }
    const bool counted = state.running.Find(SuperblockOffset / block_size) != nullptr;
    if (Footprint(state.running.Count() + (counted ? 0 : 1), state.revokes.Count()) >=
        state.limits.soft)
    {
        return true;
    }
    if (state.running_since)
    {
        const std::optional<std::uint64_t> now = Host::WallClockNanoseconds();
        return now && *now >= *state.running_since + CommitIntervalNanoseconds;
    }
    return false;
}

Status Ext2Filesystem::BeginStep()
{
    if (journal->aborted)
    {
        return std::unexpected(Error::ReadOnly);
    }
    if (CommitDue())
    {
        if (const Status committed = CommitTransaction(); !committed)
        {
            return committed;
        }
    }
    journal->step_superblock = superblock;
    journal->step_new = 0;
    journal->overflowed = false;
    journal->overflowed_alone = false;
    journal->step_orphans_added = journal->activity.orphans_added;
    return {};
}

bool Ext2Filesystem::OverflowedAlone() const
{
    return journal != nullptr && journal->overflowed_alone && !journal->aborted;
}

Status Ext2Filesystem::EndStep()
{
    JournalState& state = *journal;
    if (state.overflowed)
    {
        return std::unexpected(Error::NoSpace);
    }
    // Everything that can fail comes before anything of the running transaction changes.
    if (const Status written = WriteGroups(); !written)
    {
        return written;
    }
    const std::uint64_t superblock_block = SuperblockOffset / block_size;
    const bool superblock_counted = state.running.Find(superblock_block) != nullptr ||
                                    state.step.Find(superblock_block) != nullptr;
    const std::size_t distinct =
        state.running.Count() + state.step_new + (superblock_counted ? 0 : 1);
    if (distinct > state.limits.hard ||
        Footprint(distinct, state.revokes.Count() + state.pending_forgets.Count()) >
            state.end - state.first - 1)
    {
        state.overflowed = true;
        return std::unexpected(Error::NoSpace);
    }
    if (!state.revokes.Reserve(state.revokes.Count() + state.pending_forgets.Count()) ||
        !state.freed_groups.Reserve(state.freed_groups.Count() + state.step_freed_groups.Count()) ||
        !state.running.Reserve(state.running.Count() + state.step.Count()))
    {
        return std::unexpected(Error::OutOfMemory);
    }
    for (std::size_t index = 0; index < state.pending_forgets.Count(); ++index)
    {
        const std::uint64_t block = state.pending_forgets.At(index);
        state.step.Drop(block);
        state.running.Drop(block);
        if (state.committed.Find(block) != nullptr)
        {
            [[maybe_unused]] const Status added = state.revokes.Add(block);
        }
    }
    for (std::size_t index = 0; index < state.step_freed_groups.Count(); ++index)
    {
        [[maybe_unused]] const Status added =
            state.freed_groups.Add(state.step_freed_groups.At(index));
    }
    [[maybe_unused]] const Status absorbed = state.running.Absorb(state.step);
    state.pending_forgets.Clear();
    state.step_freed_groups.Clear();
    state.step_new = 0;
    if (!state.running_since && state.running.Count() != 0)
    {
        state.running_since = Host::WallClockNanoseconds();
    }
    return {};
}

void Ext2Filesystem::AbortStep()
{
    JournalState& state = *journal;
    const std::uint16_t errors = superblock.state & StateErrors;
    superblock = state.step_superblock;
    superblock.state = static_cast<std::uint16_t>(superblock.state | errors);
    // The table in memory is the layered view outside a step, so the blocks the step or the
    // dirty range touched are read again from what the step did not change.
    const std::uint64_t table = superblock.first_data_block + 1;
    for (std::uint64_t index = 0; index < descriptor_blocks; ++index)
    {
        const bool dirty = index >= dirty_descriptors_first && index < dirty_descriptors_end;
        if (!dirty && state.step.Find(table + index) == nullptr)
        {
            continue;
        }
        const std::span<std::uint8_t> block =
            descriptors.subspan(static_cast<std::size_t>(index * block_size), block_size);
        Status read;
        if (const std::uint8_t* copy = state.running.Find(table + index))
        {
            CopyBytes(block.data(), copy, block_size);
        }
        else if (const std::uint8_t* older = state.committed.Find(table + index))
        {
            CopyBytes(block.data(), older, block_size);
        }
        else
        {
            read = cache.Read(table + index, 0, block, block);
        }
        if (!read)
        {
            AbortJournal("the group descriptors could not be read back");
        }
    }
    dirty_descriptors_first = 0;
    dirty_descriptors_end = 0;
    state.step.Clear();
    state.pending_forgets.Clear();
    state.step_freed_groups.Clear();
    state.step_new = 0;
    state.overflowed = false;
    state.activity.orphans_added = state.step_orphans_added;
}

Error Ext2Filesystem::FailStep(Error error, bool may_retry, bool& retry)
{
    JournalState& state = *journal;
    retry = false;
    const bool overflow = state.overflowed;
    AbortStep();
    const bool pending = state.running.Count() != 0 || state.revokes.Count() != 0;
    if (may_retry && pending && !state.aborted &&
        (overflow || (error == Error::NoSpace && state.freed_groups.Count() != 0)))
    {
        if (const Status committed = CommitTransaction(); !committed)
        {
            return committed.error();
        }
        ++state.activity.retries;
        retry = true;
        return error;
    }
    if (overflow)
    {
        state.overflowed_alone = true;
        return Error::NoSpace;
    }
    NoteError(error);
    return error;
}

void Ext2Filesystem::AbortJournal(std::string_view why)
{
    LogWarning("ext2: journal aborted, no more writes: ", why);
    read_only = true;
    if (journal != nullptr)
    {
        journal->aborted = true;
        journal->activity.aborted = true;
    }
}

Result<std::uint64_t> Ext2Filesystem::NextLogPlace(std::span<std::uint8_t> scratch)
{
    JournalState& state = *journal;
    const Result<std::uint64_t> place = LogBlockPlace(state.log, state.head, scratch);
    if (!place)
    {
        return place;
    }
    ++state.head;
    if (state.head == state.end)
    {
        state.head = state.first;
        ++state.activity.wraps;
    }
    return place;
}

Status Ext2Filesystem::WriteJournalPosition(std::uint32_t start, std::uint32_t sequence,
                                            Buffers& buffers)
{
    JournalState& state = *journal;
    const Result<std::uint64_t> place = LogBlockPlace(state.log, 0, buffers[2]);
    if (!place)
    {
        return Failure(place);
    }
    if (const Status read = cache.ReadUncached(*place, buffers[3]); !read)
    {
        return read;
    }
    state.log.header.start = start;
    state.log.header.sequence = sequence;
    EncodeJournalPosition(state.log.header, buffers[3]);
    return cache.Write(*place, buffers[3]);
}

Status Ext2Filesystem::WriteRevokeBlocks(Buffers& buffers)
{
    JournalState& state = *journal;
    const std::uint32_t incompatible = state.log.header.incompatible;
    std::size_t index = 0;
    while (index < state.revokes.Count())
    {
        JournalRevokeWriter writer(buffers[0], incompatible, state.sequence);
        while (index < state.revokes.Count() && writer.Add(state.revokes.At(index)))
        {
            ++index;
        }
        writer.Finish(state.log.seed);
        const Result<std::uint64_t> place = NextLogPlace(buffers[2]);
        if (!place)
        {
            return Failure(place);
        }
        if (const Status written = cache.Write(*place, buffers[0]); !written)
        {
            return written;
        }
    }
    return {};
}

Status Ext2Filesystem::WriteDescriptorGroups(Buffers& buffers)
{
    JournalState& state = *journal;
    const std::uint32_t incompatible = state.log.header.incompatible;
    const bool checksums = JournalHasChecksums(incompatible);
    const std::span<std::uint8_t> copy = buffers[1];
    const std::size_t total = state.running.Count();
    std::size_t index = 0;
    while (index < total)
    {
        JournalDescriptorWriter writer(buffers[0], incompatible, state.sequence,
                                       state.log.header.uuid);
        std::size_t end = index;
        for (; end < total; ++end)
        {
            CopyBytes(copy.data(), state.running.BytesAt(end), block_size);
            const bool escaped = EscapeJournalBlock(copy);
            const std::uint32_t checksum =
                checksums ? JournalTagChecksum(state.log.seed, state.sequence, copy) : 0;
            if (!writer.Add(state.running.HomeAt(end), escaped, checksum))
            {
                break;
            }
        }
        writer.Finish(state.log.seed);
        Result<std::uint64_t> place = NextLogPlace(buffers[2]);
        if (!place)
        {
            return Failure(place);
        }
        if (const Status written = cache.Write(*place, buffers[0]); !written)
        {
            return written;
        }
        for (std::size_t held = index; held < end; ++held)
        {
            CopyBytes(copy.data(), state.running.BytesAt(held), block_size);
            EscapeJournalBlock(copy);
            place = NextLogPlace(buffers[2]);
            if (!place)
            {
                return Failure(place);
            }
            if (const Status written = cache.Write(*place, copy); !written)
            {
                return written;
            }
        }
        index = end;
    }
    return {};
}

Status Ext2Filesystem::CommitTransaction()
{
    JournalState& state = *journal;
    if (state.aborted ||
        (state.running.Count() == 0 && state.revokes.Count() == 0 && !state.superblock_dirty))
    {
        return {};
    }
    Buffers buffers(block_size, 4);
    if (buffers.Empty())
    {
        return std::unexpected(Error::OutOfMemory);
    }
    // C0: the superblock, with the counts every step left in memory, joins the transaction.
    superblock.write_time = static_cast<std::uint32_t>(Now().seconds);
    if (const Status staged = WriteSuperblock(buffers[0]); !staged)
    {
        return staged;
    }
    if (const Status absorbed = state.running.Absorb(state.step); !absorbed)
    {
        return absorbed;
    }
    state.step_new = 0;
    const std::uint32_t ring = state.end - state.first;
    const std::uint32_t needed = Footprint(state.running.Count(), state.revokes.Count());
    // C1: a log without room for the transaction is written home first.
    if (std::uint64_t{needed} + state.used > ring - 1 ||
        state.committed.Count() + state.running.Count() > state.committed.Maximum())
    {
        if (const Status checkpointed = Checkpoint(); !checkpointed)
        {
            return checkpointed;
        }
        if (needed > ring - 1)
        {
            return std::unexpected(Error::NoSpace);
        }
    }
    if (!state.committed.Reserve(state.committed.Count() + state.running.Count()))
    {
        return std::unexpected(Error::OutOfMemory);
    }
    const std::uint32_t transaction = state.sequence;
    const auto fail = [&](const Status& status, std::string_view why) -> Status
    {
        AbortJournal(why);
        return status;
    };
    // C2: the first transaction after an empty log says where the log starts.
    if (state.used == 0)
    {
        if (const Status positioned = WriteJournalPosition(state.head, transaction, buffers);
            !positioned)
        {
            return fail(positioned, "the journal superblock could not be written");
        }
    }
    // C3, C4
    if (const Status written = WriteRevokeBlocks(buffers); !written)
    {
        return fail(written, "a revoke block could not be written");
    }
    if (const Status written = WriteDescriptorGroups(buffers); !written)
    {
        return fail(written, "a block of the log could not be written");
    }
    // C5: everything of the transaction, and the data it depends on, before its commit block.
    if (const Status flushed = volume.Flush(); !flushed)
    {
        return fail(flushed, "a flush failed");
    }
    // C6
    const Timestamp now = Now();
    EncodeCommitBlock(buffers[0], state.log.header.incompatible, state.log.seed, transaction,
                      now.seconds, now.nanoseconds);
    const Result<std::uint64_t> place = NextLogPlace(buffers[2]);
    if (!place)
    {
        return fail(Failure(place), "the log could not be mapped");
    }
    if (const Status written = cache.Write(*place, buffers[0]); !written)
    {
        return fail(written, "the commit block could not be written");
    }
    // C7
    if (const Status flushed = volume.Flush(); !flushed)
    {
        return fail(flushed, "a flush failed");
    }
    // C8: the log now holds the transaction.
    for (std::size_t index = 0; index < state.revokes.Count(); ++index)
    {
        state.committed.Drop(state.revokes.At(index));
    }
    const auto blocks = static_cast<std::uint32_t>(state.running.Count());
    [[maybe_unused]] const Status absorbed = state.committed.Absorb(state.running);
    state.used += needed;
    ++state.sequence;
    state.revokes.Clear();
    state.freed_groups.Clear();
    state.running_since.reset();
    state.superblock_dirty = false;
    ++state.activity.transactions;
    state.activity.largest_transaction =
        blocks > state.activity.largest_transaction ? blocks : state.activity.largest_transaction;
    // C9
    if (state.committed.Count() > state.limits.committed)
    {
        return Checkpoint();
    }
    return {};
}

Status Ext2Filesystem::Checkpoint()
{
    JournalState& state = *journal;
    if (state.aborted || (state.committed.Count() == 0 && state.used == 0))
    {
        return {};
    }
    const std::size_t count = state.committed.Count();
    Buffers buffers(block_size, 4);
    const Allocation order(count * sizeof(std::uint32_t));
    auto* indexes = static_cast<std::uint32_t*>(order.memory);
    if (buffers.Empty() || (count != 0 && indexes == nullptr))
    {
        return std::unexpected(Error::OutOfMemory);
    }
    SortByHome(state.committed, indexes, count);
    const auto fail = [&](const Status& status, std::string_view why) -> Status
    {
        AbortJournal(why);
        return status;
    };
    // K1
    for (std::size_t position = 0; position < count; ++position)
    {
        const std::size_t index = indexes[position];
        const std::span<const std::uint8_t> copy(state.committed.BytesAt(index), block_size);
        if (const Status written = cache.Write(state.committed.HomeAt(index), copy); !written)
        {
            return fail(written, "a block could not be written to its place");
        }
    }
    // K2
    if (const Status flushed = volume.Flush(); !flushed)
    {
        return fail(flushed, "a flush failed");
    }
    // K3
    if (const Status positioned = WriteJournalPosition(0, state.sequence, buffers); !positioned)
    {
        return fail(positioned, "the journal superblock could not be written");
    }
    // K4
    if (const Status flushed = volume.Flush(); !flushed)
    {
        return fail(flushed, "a flush failed");
    }
    state.committed.Clear();
    state.used = 0;
    ++state.activity.checkpoints;
    return {};
}

Status Ext2Filesystem::CloseJournal()
{
    if (journal == nullptr || journal->aborted || !writable_mount)
    {
        return {};
    }
    // U0
    if (const Status committed = CommitTransaction(); !committed)
    {
        return committed;
    }
    if (journal->aborted)
    {
        return {};
    }
    if (const Status checkpointed = Checkpoint(); !checkpointed)
    {
        return checkpointed;
    }
    // U1: the log is empty on disk, so the next mount has nothing to replay.
    Buffers buffers(block_size, 1);
    if (buffers.Empty())
    {
        return std::unexpected(Error::OutOfMemory);
    }
    const Superblock before = superblock;
    superblock.incompatible &= ~IncompatibleRecover;
    superblock.write_time = static_cast<std::uint32_t>(Now().seconds);
    Status written = EncodeSuperblockBlock(buffers[0], true);
    if (written)
    {
        written = cache.Write(SuperblockOffset / block_size, buffers[0]);
    }
    if (!written)
    {
        superblock = before;
        AbortJournal("the superblock could not be written");
        return written;
    }
    // U2
    if (const Status flushed = volume.Flush(); !flushed)
    {
        AbortJournal("a flush failed");
        return flushed;
    }
    writable_mount = false;
    read_only = true;
    DestroyJournal();
    return {};
}

Status Ext2Filesystem::StartJournal()
{
    const auto refuse = [&](std::string_view reason) -> Status
    {
        LogWarning("ext2: mounted read-only: ", reason);
        read_only = true;
        DestroyJournal();
        return {};
    };
    if (superblock.journal_inode == 0)
    {
        return refuse("the journal is on another device");
    }
    void* memory = Host::Allocate(sizeof(JournalState));
    if (memory == nullptr)
    {
        return std::unexpected(Error::OutOfMemory);
    }
    journal = new (memory) JournalState(block_size);
    JournalState& state = *journal;
    Buffers buffers(block_size, 3);
    if (buffers.Empty())
    {
        return std::unexpected(Error::OutOfMemory);
    }
    if (const Status opened = OpenJournal(state.log, buffers, false); !opened)
    {
        if (opened.error() == Error::Unsupported)
        {
            return refuse("unsupported journal features");
        }
        if (opened.error() != Error::Corrupt)
        {
            return opened;
        }
        return refuse("the journal superblock does not hold together; run e2fsck");
    }
    JournalSuperblock& header = state.log.header;
    if (header.header.block_type == JournalSuperblockVersion1)
    {
        return refuse("a version 1 journal cannot hold revoke records");
    }
    if (header.maximum_length < MinimumJournalBlocks)
    {
        return refuse("the journal is shorter than 1024 blocks");
    }
    if (header.start != 0)
    {
        return refuse("the journal holds transactions but needs_recovery is clear; run e2fsck");
    }
    // Every position of the ring must have a block of its own.
    for (std::uint32_t position = header.first; position < header.maximum_length; ++position)
    {
        const Result<std::uint64_t> place = LogBlockPlace(state.log, position, buffers[2]);
        if (!place)
        {
            if (place.error() != Error::Corrupt)
            {
                return Failure(place);
            }
            return refuse("the journal superblock does not hold together; run e2fsck");
        }
        if (!IsDataBlock(*place))
        {
            return refuse("the journal superblock does not hold together; run e2fsck");
        }
    }
    // The features a writer needs, as Linux sets them at a mount, with the log empty.
    const std::uint32_t compatible = header.compatible & ~JournalCompatibleChecksum;
    std::uint32_t incompatible = header.incompatible | JournalIncompatibleRevoke;
    if ((superblock.incompatible & Incompatible64Bit) != 0)
    {
        incompatible |= JournalIncompatible64Bit;
    }
    if (metadata_checksums && !JournalHasChecksums(incompatible))
    {
        incompatible |= JournalIncompatibleChecksumV3;
    }
    if (compatible != header.compatible || incompatible != header.incompatible)
    {
        header.compatible = compatible;
        header.incompatible = incompatible;
        const Result<std::uint64_t> place = LogBlockPlace(state.log, 0, buffers[2]);
        if (!place)
        {
            return Failure(place);
        }
        Status written = cache.ReadUncached(*place, buffers[0]);
        if (written)
        {
            EncodeJournalFeatures(header, buffers[0]);
            written = cache.Write(*place, buffers[0]);
        }
        if (written)
        {
            written = volume.Flush();
        }
        if (!written)
        {
            if (written.error() == Error::ReadOnly)
            {
                return refuse("the device is read-only");
            }
            return written;
        }
    }
    state.log.seed = JournalChecksumSeed(header);
    state.first = header.first;
    state.end = header.maximum_length;
    state.head = header.first;
    state.used = 0;
    state.sequence = header.sequence + 1;
    const std::uint32_t ring = state.end - state.first;
    const auto memory_blocks = static_cast<std::uint32_t>(state.step.Maximum());
    state.limits.hard = ring - 1 < memory_blocks ? ring - 1 : memory_blocks;
    state.limits.soft = ring / 4 < memory_blocks / 2 ? ring / 4 : memory_blocks / 2;
    state.limits.committed = memory_blocks;
    return {};
}

} // namespace FeatherDisk::Ext
