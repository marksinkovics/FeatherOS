// SPDX-License-Identifier: MIT OR Apache-2.0
// Orphans, and the changes that do not fit in one transaction. An inode a change has begun and a
// crash would leave unfinished is listed first, in a slot of the orphan file or in the
// s_last_orphan list, and unlisted by the transaction that finishes the change. A change too big
// for one transaction is then a series of transactions between those two:
//   - a truncation stores the new size and lists the inode, frees the blocks from the end in steps
//     of ever fewer blocks, and unlists it with the last of them;
//   - a removal drops the name and the last link and lists the inode, frees its blocks the same
//     way, and frees the inode and unlists it in one step;
//   - a write past the end lists the inode with its old size, writes in chunks, and stores the
//     new size with the last one.
// A mount that may write finishes whatever the list or the file holds, ours and Linux's: an
// unlinked inode is released, a linked file is cut back to its size.
#include "common/bitmap.h"
#include "common/bytes.h"
#include "common/little_endian.h"
#include "ext/ext2_filesystem.h"
#include "ext/transaction.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace FeatherDisk::Ext
{
namespace
{

constexpr std::size_t SlotSize = 4;
constexpr std::string_view FileDamaged = "the orphan file does not hold together; run e2fsck";
constexpr std::string_view ListDamaged = "the orphan list does not hold together; run e2fsck";

std::uint32_t LoadSlot(std::span<const std::uint8_t> block, std::size_t slot)
{
    return LoadLittleEndian<std::uint32_t>(block.data() + slot * SlotSize);
}

void StoreSlot(std::span<std::uint8_t> block, std::size_t slot, std::uint32_t value)
{
    StoreLittleEndian(block.data() + slot * SlotSize, value);
}

} // namespace

std::size_t OrphansPerBlock(std::uint32_t block_size)
{
    return (block_size - OrphanTailSize) / SlotSize;
}

bool Ext2Filesystem::HasOrphanFile() const
{
    return (superblock.compatible & CompatibleOrphanFile) != 0;
}

void Ext2Filesystem::NoteSplit()
{
    if (journal != nullptr)
    {
        ++journal->activity.splits;
    }
}

Result<Inode> Ext2Filesystem::LoadOrphanFile(std::span<std::uint8_t> scratch)
{
    const std::uint32_t number = superblock.orphan_file_inode;
    if (number < superblock.first_inode || number > superblock.inodes_count)
    {
        return std::unexpected(Error::Corrupt);
    }
    const Result<Node> node = LoadNode(number, scratch);
    if (!node)
    {
        return std::unexpected(node.error() == Error::NotFound ? Error::Corrupt : node.error());
    }
    if (node->type != NodeType::File || (node->inode.flags & InodeInlineData) != 0 ||
        node->inode.size == 0 || node->inode.size % block_size != 0)
    {
        return std::unexpected(Error::Corrupt);
    }
    return node->inode;
}

Result<std::uint64_t> Ext2Filesystem::OrphanBlockPlace(const Inode& file, std::uint64_t logical,
                                                       std::span<std::uint8_t> scratch)
{
    const Result<std::uint64_t> place = PhysicalBlock(file, logical, scratch);
    if (!place)
    {
        return place;
    }
    if (*place == 0 || !IsDataBlock(*place))
    {
        return std::unexpected(Error::Corrupt);
    }
    return place;
}

bool Ext2Filesystem::OrphanBlockMatches(const Inode& file, std::uint64_t place,
                                        std::span<const std::uint8_t> block) const
{
    if (LoadLittleEndian<std::uint32_t>(block.data() + block_size - OrphanTailSize) !=
        OrphanBlockMagic)
    {
        return false;
    }
    return !metadata_checksums ||
           LoadLittleEndian<std::uint32_t>(block.data() + block_size - 4) ==
               OrphanBlockChecksum(volume_seed, superblock.orphan_file_inode, file.generation,
                                   place, block.first(block_size - OrphanTailSize));
}

void Ext2Filesystem::StoreOrphanTail(const Inode& file, std::uint64_t place,
                                     std::span<std::uint8_t> block) const
{
    if (metadata_checksums)
    {
        StoreLittleEndian(block.data() + block_size - 4,
                          OrphanBlockChecksum(volume_seed, superblock.orphan_file_inode,
                                              file.generation, place,
                                              block.first(block_size - OrphanTailSize)));
    }
}

Result<OrphanEntry> Ext2Filesystem::AddOrphan(NodeId node, Inode& inode)
{
    if (HasOrphanFile())
    {
        Buffers buffers(block_size, 2);
        if (buffers.Empty())
        {
            return std::unexpected(Error::OutOfMemory);
        }
        const std::span<std::uint8_t> block = buffers[DataBuffer];
        const std::span<std::uint8_t> scratch = buffers[ScratchBuffer];
        const Result<Inode> file = LoadOrphanFile(scratch);
        if (!file)
        {
            return Failure(file);
        }
        const std::uint64_t blocks = file->size / block_size;
        for (std::uint64_t logical = 0; logical < blocks; ++logical)
        {
            const Result<std::uint64_t> place = OrphanBlockPlace(*file, logical, scratch);
            if (!place)
            {
                return Failure(place);
            }
            if (const Status read = ReadMetadata(*place, 0, block, scratch); !read)
            {
                return Failure(read);
            }
            if (!OrphanBlockMatches(*file, *place, block))
            {
                return std::unexpected(Error::Corrupt);
            }
            for (std::size_t slot = 0; slot < OrphansPerBlock(block_size); ++slot)
            {
                if (LoadSlot(block, slot) != 0)
                {
                    continue;
                }
                StoreSlot(block, slot, static_cast<std::uint32_t>(node));
                StoreOrphanTail(*file, *place, block);
                if (const Status written = WriteMetadata(*place, block); !written)
                {
                    return Failure(written);
                }
                superblock.read_only_compatible |= ReadOnlyCompatibleOrphanPresent;
                ++journal->activity.orphans_added;
                return OrphanEntry{.place = OrphanPlace::File, .logical = logical, .slot = slot};
            }
        }
    }
    // Without the orphan file, or with it full, as Linux does.
    inode.deletion_time = superblock.last_orphan;
    superblock.last_orphan = static_cast<std::uint32_t>(node);
    ++journal->activity.orphans_added;
    return OrphanEntry{.place = OrphanPlace::List, .logical = 0, .slot = 0};
}

Status Ext2Filesystem::UpdateOrphanPresent()
{
    Buffers buffers(block_size, 2);
    if (buffers.Empty())
    {
        return std::unexpected(Error::OutOfMemory);
    }
    const std::span<std::uint8_t> block = buffers[DataBuffer];
    const std::span<std::uint8_t> scratch = buffers[ScratchBuffer];
    const Result<Inode> file = LoadOrphanFile(scratch);
    if (!file)
    {
        return Failure(file);
    }
    const std::uint64_t blocks = file->size / block_size;
    for (std::uint64_t logical = 0; logical < blocks; ++logical)
    {
        const Result<std::uint64_t> place = OrphanBlockPlace(*file, logical, scratch);
        if (!place)
        {
            return Failure(place);
        }
        if (const Status read = ReadMetadata(*place, 0, block, scratch); !read)
        {
            return read;
        }
        for (std::size_t slot = 0; slot < OrphansPerBlock(block_size); ++slot)
        {
            if (LoadSlot(block, slot) != 0)
            {
                return {};
            }
        }
    }
    if ((superblock.read_only_compatible & ReadOnlyCompatibleOrphanPresent) != 0)
    {
        superblock.read_only_compatible &= ~ReadOnlyCompatibleOrphanPresent;
        journal->superblock_dirty = true;
    }
    return {};
}

Status Ext2Filesystem::RemoveOrphan(NodeId node, Inode& inode, const OrphanEntry& entry)
{
    if (entry.place == OrphanPlace::List)
    {
        // Our entry is the head: changes run one at a time and processing empties the list.
        if (superblock.last_orphan != node)
        {
            return std::unexpected(Error::Corrupt);
        }
        superblock.last_orphan = inode.deletion_time;
        inode.deletion_time = 0;
        return {};
    }
    Buffers buffers(block_size, 2);
    if (buffers.Empty())
    {
        return std::unexpected(Error::OutOfMemory);
    }
    const std::span<std::uint8_t> block = buffers[DataBuffer];
    const std::span<std::uint8_t> scratch = buffers[ScratchBuffer];
    const Result<Inode> file = LoadOrphanFile(scratch);
    if (!file)
    {
        return Failure(file);
    }
    const Result<std::uint64_t> place = OrphanBlockPlace(*file, entry.logical, scratch);
    if (!place)
    {
        return Failure(place);
    }
    if (const Status read = ReadMetadata(*place, 0, block, scratch); !read)
    {
        return read;
    }
    if (!OrphanBlockMatches(*file, *place, block) || entry.slot >= OrphansPerBlock(block_size) ||
        LoadSlot(block, entry.slot) != node)
    {
        return std::unexpected(Error::Corrupt);
    }
    StoreSlot(block, entry.slot, 0);
    StoreOrphanTail(*file, *place, block);
    if (const Status written = WriteMetadata(*place, block); !written)
    {
        return written;
    }
    return UpdateOrphanPresent();
}

Status Ext2Filesystem::CheckOrphans(Buffers& buffers)
{
    const std::span<std::uint8_t> block = buffers[DataBuffer];
    const std::span<std::uint8_t> scratch = buffers[ScratchBuffer];
    const std::span<std::uint8_t> bitmap = buffers[BitmapBuffer];
    const auto damaged = [](std::string_view why) -> Status
    {
        LogWarning("ext2: mounted read-only: ", why);
        return std::unexpected(Error::Corrupt);
    };
    const auto listed = [&](std::uint32_t node) -> Result<bool>
    {
        if (node < superblock.first_inode || node > superblock.inodes_count)
        {
            return false;
        }
        if (const Status loaded = LoadInodeBitmap(GroupOfInode(node), bitmap); !loaded)
        {
            return Failure(loaded);
        }
        return TestBit(bitmap, (node - 1) % superblock.inodes_per_group);
    };
    if (HasOrphanFile())
    {
        const Result<Inode> file = LoadOrphanFile(scratch);
        if (!file)
        {
            return file.error() == Error::Corrupt ? damaged(FileDamaged) : Failure(file);
        }
        const std::uint64_t blocks = file->size / block_size;
        for (std::uint64_t logical = 0; logical < blocks; ++logical)
        {
            const Result<std::uint64_t> place = OrphanBlockPlace(*file, logical, scratch);
            if (!place)
            {
                return place.error() == Error::Corrupt ? damaged(FileDamaged) : Failure(place);
            }
            if (const Status read = ReadMetadata(*place, 0, block, scratch); !read)
            {
                return read;
            }
            if (!OrphanBlockMatches(*file, *place, block))
            {
                return damaged(FileDamaged);
            }
            for (std::size_t slot = 0; slot < OrphansPerBlock(block_size); ++slot)
            {
                const std::uint32_t node = LoadSlot(block, slot);
                if (node == 0)
                {
                    continue;
                }
                const Result<bool> in_use = listed(node);
                if (!in_use)
                {
                    return in_use.error() == Error::Corrupt ? damaged(ListDamaged)
                                                            : Failure(in_use);
                }
                if (!*in_use)
                {
                    return damaged(ListDamaged);
                }
            }
        }
    }
    std::uint32_t node = superblock.last_orphan;
    for (std::uint32_t steps = 0; node != 0; ++steps)
    {
        if (steps >= superblock.inodes_count)
        {
            return damaged(ListDamaged);
        }
        const Result<bool> in_use = listed(node);
        if (!in_use)
        {
            return in_use.error() == Error::Corrupt ? damaged(ListDamaged) : Failure(in_use);
        }
        if (!*in_use)
        {
            return damaged(ListDamaged);
        }
        const Result<Inode> inode = LoadInode(node, scratch);
        if (!inode)
        {
            return inode.error() == Error::Corrupt ? damaged(ListDamaged) : Failure(inode);
        }
        node = inode->deletion_time;
    }
    return {};
}

Status Ext2Filesystem::ProcessOrphan(NodeId node, const OrphanEntry& entry)
{
    Buffers buffers(block_size, 1);
    if (buffers.Empty())
    {
        return std::unexpected(Error::OutOfMemory);
    }
    const Result<Inode> loaded = LoadInode(node, buffers[0]);
    if (!loaded)
    {
        return Failure(loaded);
    }
    const std::optional<NodeType> type = TypeOf(loaded->mode);
    if (!type)
    {
        return std::unexpected(Error::Corrupt);
    }
    Status status;
    if (loaded->link_count == 0)
    {
        status = ReleaseSplit(node, entry);
    }
    else if (*type == NodeType::File && (loaded->flags & InodeInlineData) == 0)
    {
        status = ShrinkSplit(node, DivideRoundingUp(loaded->size, block_size), &entry);
    }
    else
    {
        LogWithValue("ext2: a linked orphan that is not a file is only unlisted, inode ",
                     static_cast<std::uint32_t>(node));
        status = Journalled(
            [&]() -> Status
            {
                Buffers inner(block_size, 1);
                if (inner.Empty())
                {
                    return std::unexpected(Error::OutOfMemory);
                }
                Result<Inode> inode = LoadInode(node, inner[0]);
                if (!inode)
                {
                    return Failure(inode);
                }
                if (const Status removed = RemoveOrphan(node, *inode, entry); !removed)
                {
                    return removed;
                }
                return StoreInode(node, *inode, false, inner[0]);
            });
    }
    if (status)
    {
        ++journal->activity.orphans_processed;
    }
    return status;
}

Status Ext2Filesystem::ProcessOrphans()
{
    const Status status = [&]() -> Status
    {
        Buffers buffers(block_size, 2);
        if (buffers.Empty())
        {
            return std::unexpected(Error::OutOfMemory);
        }
        const std::span<std::uint8_t> block = buffers[DataBuffer];
        const std::span<std::uint8_t> scratch = buffers[ScratchBuffer];
        if (HasOrphanFile())
        {
            const Result<Inode> file = LoadOrphanFile(scratch);
            if (!file)
            {
                return Failure(file);
            }
            const std::uint64_t blocks = file->size / block_size;
            for (std::uint64_t logical = 0; logical < blocks; ++logical)
            {
                const Result<std::uint64_t> place = OrphanBlockPlace(*file, logical, scratch);
                if (!place)
                {
                    return Failure(place);
                }
                if (const Status read = ReadMetadata(*place, 0, block, scratch); !read)
                {
                    return read;
                }
                for (std::size_t slot = 0; slot < OrphansPerBlock(block_size); ++slot)
                {
                    const std::uint32_t node = LoadSlot(block, slot);
                    if (node == 0)
                    {
                        continue;
                    }
                    const OrphanEntry entry = {
                        .place = OrphanPlace::File, .logical = logical, .slot = slot};
                    if (const Status processed = ProcessOrphan(node, entry); !processed)
                    {
                        return processed;
                    }
                    if (const Status read = ReadMetadata(*place, 0, block, scratch); !read)
                    {
                        return read;
                    }
                }
            }
        }
        for (std::uint32_t steps = 0; superblock.last_orphan != 0; ++steps)
        {
            if (steps >= superblock.inodes_count)
            {
                return std::unexpected(Error::Corrupt);
            }
            const OrphanEntry entry = {.place = OrphanPlace::List, .logical = 0, .slot = 0};
            if (const Status processed = ProcessOrphan(superblock.last_orphan, entry); !processed)
            {
                return processed;
            }
        }
        if (HasOrphanFile() &&
            (superblock.read_only_compatible & ReadOnlyCompatibleOrphanPresent) != 0)
        {
            if (const Status cleared = Journalled([&] { return UpdateOrphanPresent(); }); !cleared)
            {
                return cleared;
            }
        }
        return CommitTransaction();
    }();
    if (!status && (status.error() == Error::Corrupt || status.error() == Error::NoSpace ||
                    status.error() == Error::Unsupported))
    {
        LogWarning("ext2: mounted read-only: ", "orphan inodes could not be finished; run e2fsck");
        read_only = true;
        return {};
    }
    return status;
}

Status Ext2Filesystem::ShrinkTo(NodeId node, Inode& inode, std::uint64_t keep, Buffers& buffers)
{
    if ((inode.flags & InodeExtents) == 0)
    {
        return ShrinkBlocks(node, inode, keep * block_size, true, buffers);
    }
    const Status shrunk = ShrinkExtents(node, inode, keep, buffers);
    const Status stored = StoreInode(node, inode, false, buffers[ScratchBuffer]);
    return !shrunk ? shrunk : stored;
}

Status Ext2Filesystem::ShrinkPiece(NodeId node, std::uint64_t keep, const OrphanEntry* entry)
{
    Buffers buffers(block_size, ShrinkBuffers);
    if (buffers.Empty())
    {
        return std::unexpected(Error::OutOfMemory);
    }
    const std::span<std::uint8_t> scratch = buffers[ScratchBuffer];
    const Result<Inode> loaded = LoadInode(node, scratch);
    if (!loaded)
    {
        return Failure(loaded);
    }
    Inode inode = *loaded;
    if (const Status shrunk = ShrinkTo(node, inode, keep, buffers); !shrunk)
    {
        return shrunk;
    }
    if (entry == nullptr)
    {
        return {};
    }
    if (const Status removed = RemoveOrphan(node, inode, *entry); !removed)
    {
        return removed;
    }
    return StoreInode(node, inode, false, scratch);
}

Status Ext2Filesystem::ShrinkSplit(NodeId node, std::uint64_t keep, const OrphanEntry* entry)
{
    std::uint64_t end = 0;
    {
        Buffers buffers(block_size, 1);
        if (buffers.Empty())
        {
            return std::unexpected(Error::OutOfMemory);
        }
        const Result<Inode> inode = LoadInode(node, buffers[0]);
        if (!inode)
        {
            return Failure(inode);
        }
        const Result<std::uint64_t> mapped = MappedEnd(*inode);
        if (!mapped)
        {
            return Failure(mapped);
        }
        end = *mapped;
    }
    std::uint64_t current = end > keep ? end : keep;
    const std::uint64_t widest = current - keep;
    std::uint64_t window = widest;
    for (;;)
    {
        const bool last = current - keep <= window;
        const std::uint64_t target = last ? keep : current - window;
        const Status status =
            Journalled([&] { return ShrinkPiece(node, target, last ? entry : nullptr); });
        if (status)
        {
            if (last)
            {
                return {};
            }
            current = target;
            // A piece that fits says little about the next: a block in the middle of an indirect
            // block costs more than a whole one, so the window grows again.
            window = window * 2 < widest ? window * 2 : widest;
        }
        else if (status.error() == Error::NoSpace && OverflowedAlone() && window > 1)
        {
            window /= 2;
        }
        else
        {
            return status;
        }
    }
}

Status Ext2Filesystem::FinishRelease(NodeId node, const OrphanEntry& entry)
{
    Buffers buffers(block_size, ChangeBuffers);
    if (buffers.Empty())
    {
        return std::unexpected(Error::OutOfMemory);
    }
    const std::span<std::uint8_t> scratch = buffers[ScratchBuffer];
    const Result<Inode> loaded = LoadInode(node, scratch);
    if (!loaded)
    {
        return Failure(loaded);
    }
    const std::optional<NodeType> type = TypeOf(loaded->mode);
    if (!type || loaded->link_count != 0)
    {
        return std::unexpected(Error::Corrupt);
    }
    if (const Status releasable = CheckReleasable(*loaded, buffers[BitmapBuffer]); !releasable)
    {
        return releasable;
    }
    Inode inode = *loaded;
    if (const Status removed = RemoveOrphan(node, inode, entry); !removed)
    {
        return removed;
    }
    const Timestamp now = Now();
    inode.change_time = now;
    inode.deletion_time = static_cast<std::uint32_t>(now.seconds);
    if (const Status stored = StoreInode(node, inode, false, scratch); !stored)
    {
        return stored;
    }
    if (const Status released = ReleaseContents({.inode = inode, .type = *type}, buffers);
        !released)
    {
        return released;
    }
    return FreeInode(node, *type == NodeType::Directory, buffers[BitmapBuffer]);
}

Status Ext2Filesystem::ReleaseSplit(NodeId node, const OrphanEntry& entry)
{
    {
        Buffers buffers(block_size, 1);
        if (buffers.Empty())
        {
            return std::unexpected(Error::OutOfMemory);
        }
        const Result<Inode> inode = LoadInode(node, buffers[0]);
        if (!inode)
        {
            return Failure(inode);
        }
        const std::optional<NodeType> type = TypeOf(inode->mode);
        if (!type)
        {
            return std::unexpected(Error::Corrupt);
        }
        // Whatever is not a file with a block map or extents goes with the inode, in one step.
        if (*type == NodeType::File && (inode->flags & InodeInlineData) == 0)
        {
            if (const Status shrunk = ShrinkSplit(node, 0, nullptr); !shrunk)
            {
                return shrunk;
            }
        }
    }
    return Journalled([&] { return FinishRelease(node, entry); });
}

Status Ext2Filesystem::TruncateSplit(NodeId file, std::uint64_t size)
{
    NoteSplit();
    std::optional<OrphanEntry> entry;
    const Status first = Journalled(
        [&]() -> Status
        {
            entry.reset();
            Buffers buffers(block_size, ChangeBuffers);
            if (buffers.Empty())
            {
                return std::unexpected(Error::OutOfMemory);
            }
            const std::span<std::uint8_t> scratch = buffers[ScratchBuffer];
            const Result<Node> node = LoadNode(file, scratch);
            if (!node)
            {
                return Failure(node);
            }
            Inode inode = node->inode;
            if (node->type != NodeType::File || (inode.flags & InodeInlineData) != 0 ||
                size >= inode.size)
            {
                return std::unexpected(Error::NoSpace);
            }
            if (const Status zeroed = ZeroFrom(inode, size, scratch); !zeroed)
            {
                return zeroed;
            }
            const Timestamp now = Now();
            inode.size = size;
            inode.modify_time = now;
            inode.change_time = now;
            const Result<OrphanEntry> added = AddOrphan(file, inode);
            if (!added)
            {
                return Failure(added);
            }
            entry = *added;
            return StoreInode(file, inode, false, scratch);
        });
    if (!first || !entry)
    {
        return first;
    }
    return ShrinkSplit(file, DivideRoundingUp(size, block_size), &*entry);
}

Status Ext2Filesystem::FinishWriteSplit(NodeId file, std::uint64_t size, const OrphanEntry& entry)
{
    return Journalled(
        [&]() -> Status
        {
            Buffers buffers(block_size, 1);
            if (buffers.Empty())
            {
                return std::unexpected(Error::OutOfMemory);
            }
            Result<Inode> inode = LoadInode(file, buffers[0]);
            if (!inode)
            {
                return Failure(inode);
            }
            if (size > inode->size)
            {
                inode->size = size;
                const Timestamp now = Now();
                inode->modify_time = now;
                inode->change_time = now;
                if (const Status large = SetLargeFile(size, buffers[0]); !large)
                {
                    return large;
                }
            }
            if (const Status removed = RemoveOrphan(file, *inode, entry); !removed)
            {
                return removed;
            }
            return StoreInode(file, *inode, false, buffers[0]);
        });
}

Result<std::size_t> Ext2Filesystem::WriteSplit(NodeId file, std::uint64_t offset,
                                               std::span<const std::uint8_t> data)
{
    NoteSplit();
    std::uint64_t size = 0;
    {
        Buffers buffers(block_size, 1);
        if (buffers.Empty())
        {
            return std::unexpected(Error::OutOfMemory);
        }
        const Result<Node> node = LoadNode(file, buffers[0]);
        if (!node)
        {
            return Failure(node);
        }
        size = node->inode.size;
    }
    const bool extending = offset + data.size() > size;
    std::optional<OrphanEntry> entry;
    bool finished = false;
    std::size_t done = 0;
    std::size_t chunk = data.size() / 2 / block_size * block_size;
    chunk = chunk < block_size ? block_size : chunk;
    Error failure = Error::NoSpace;
    while (done < data.size())
    {
        const std::size_t length = chunk < data.size() - done ? chunk : data.size() - done;
        const bool last = done + length == data.size();
        std::optional<OrphanEntry> added;
        const Result<std::size_t> piece = Journalled(
            [&]() -> Result<std::size_t>
            {
                added.reset();
                Buffers buffers(block_size, 1);
                if (buffers.Empty())
                {
                    return std::unexpected(Error::OutOfMemory);
                }
                if (extending && !entry)
                {
                    Result<Inode> inode = LoadInode(file, buffers[0]);
                    if (!inode)
                    {
                        return Failure(inode);
                    }
                    const Result<OrphanEntry> listed = AddOrphan(file, *inode);
                    if (!listed)
                    {
                        return Failure(listed);
                    }
                    if (const Status stored = StoreInode(file, *inode, false, buffers[0]); !stored)
                    {
                        return Failure(stored);
                    }
                    added = *listed;
                }
                const Result<std::size_t> written = WriteFile(
                    file, offset + done, data.subspan(done, length), extending && !last, done != 0);
                if (!written || !extending || !last)
                {
                    return written;
                }
                Result<Inode> inode = LoadInode(file, buffers[0]);
                if (!inode)
                {
                    return Failure(inode);
                }
                if (const Status removed = RemoveOrphan(file, *inode, entry ? *entry : *added);
                    !removed)
                {
                    return Failure(removed);
                }
                if (const Status stored = StoreInode(file, *inode, false, buffers[0]); !stored)
                {
                    return Failure(stored);
                }
                return written;
            });
        if (piece)
        {
            if (added)
            {
                entry = added;
            }
            done += *piece;
            finished = extending && last;
            if (*piece < length)
            {
                break;
            }
        }
        else if (piece.error() == Error::NoSpace && OverflowedAlone() && chunk > block_size)
        {
            chunk = chunk / 2 / block_size * block_size;
            chunk = chunk < block_size ? block_size : chunk;
        }
        else
        {
            failure = piece.error();
            break;
        }
    }
    if (extending && entry && !finished)
    {
        const Status closed = FinishWriteSplit(file, offset + done, *entry);
        if (!closed && done == 0)
        {
            return std::unexpected(closed.error());
        }
        if (!closed)
        {
            failure = closed.error();
        }
    }
    if (done == 0)
    {
        return std::unexpected(failure);
    }
    return done;
}

} // namespace FeatherDisk::Ext
