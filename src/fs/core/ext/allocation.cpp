// SPDX-License-Identifier: MIT OR Apache-2.0
// Block groups and their bitmaps. A volume is cut into groups of blocks_per_group blocks and
// inodes_per_group inodes; each group has one block of bitmap for its blocks and one for its
// inodes, and its descriptor counts what is free. The descriptors stay in memory and go back to
// the volume on Sync; a bitmap is written as soon as it changes, so a block or inode is marked
// before anything points to it. With group checksums (gdt_csum or metadata_csum) a group may
// never have been written: BLOCK_UNINIT leaves its block bitmap to be worked out from its
// metadata, INODE_UNINIT its inode bitmap and table, and bg_itable_unused the end of its table.
// The first use writes the bitmap, then the descriptor with the flag cleared or the count
// lowered, before anything uses what they cover. Bitmap checksums (metadata_csum) are checked as
// the bitmaps load and kept in the descriptors, which Sync writes.
#include "common/bitmap.h"
#include "common/bytes.h"
#include "common/little_endian.h"
#include "ext/ext2_filesystem.h"

namespace FeatherDisk::Ext
{
namespace
{

constexpr std::size_t DescriptorChecksumOffset = 30;

} // namespace

std::uint32_t Ext2Filesystem::BlocksInGroup(std::uint32_t group) const
{
    // The last group ends with the volume.
    const std::uint64_t start = std::uint64_t{group} * superblock.blocks_per_group;
    const std::uint64_t remaining = superblock.blocks_count - superblock.first_data_block - start;
    return remaining < superblock.blocks_per_group ? static_cast<std::uint32_t>(remaining)
                                                   : superblock.blocks_per_group;
}

std::uint64_t Ext2Filesystem::GroupFirstBlock(std::uint32_t group) const
{
    return superblock.first_data_block + std::uint64_t{group} * superblock.blocks_per_group;
}

std::uint32_t Ext2Filesystem::GroupOfInode(NodeId node) const
{
    return static_cast<std::uint32_t>((node - 1) / superblock.inodes_per_group);
}

GroupDescriptor Ext2Filesystem::Group(std::uint32_t group) const
{
    return DecodeGroupDescriptor(
        descriptors.subspan(std::size_t{group} * descriptor_size, descriptor_size));
}

void Ext2Filesystem::StoreGroup(std::uint32_t group, const GroupDescriptor& descriptor)
{
    const std::size_t offset = std::size_t{group} * descriptor_size;
    const std::span<std::uint8_t> bytes = descriptors.subspan(offset, descriptor_size);
    EncodeGroupDescriptor(descriptor, bytes);
    if (group_checksums)
    {
        StoreLittleEndian(bytes.data() + DescriptorChecksumOffset,
                          GroupDescriptorChecksum(superblock, volume_seed, group, bytes));
    }
    const std::uint64_t block = offset / block_size;
    if (dirty_descriptors_first == dirty_descriptors_end)
    {
        dirty_descriptors_first = block;
        dirty_descriptors_end = block + 1;
        return;
    }
    dirty_descriptors_first = block < dirty_descriptors_first ? block : dirty_descriptors_first;
    dirty_descriptors_end = block + 1 > dirty_descriptors_end ? block + 1 : dirty_descriptors_end;
}

// The bitmaps and inode table of the group or, with flex_bg, of any group of its flexible group,
// which keeps them together in its first groups, and in a group with a backup, the copies of the
// superblock, the descriptor table and the blocks reserved for its growth. Only data may be
// allocated or freed.
bool Ext2Filesystem::IsGroupMetadata(std::uint32_t group, std::uint64_t block) const
{
    const std::uint64_t first = GroupFirstBlock(group);
    if (GroupHasSuperblock(superblock, group) && block >= first &&
        block < first + 1 + descriptor_blocks + superblock.reserved_descriptor_blocks)
    {
        return true;
    }
    std::uint64_t from = group;
    std::uint64_t to = std::uint64_t{group} + 1;
    if ((superblock.incompatible & IncompatibleFlexibleGroups) != 0 &&
        superblock.log_groups_per_flex < 32)
    {
        const std::uint64_t size = std::uint64_t{1} << superblock.log_groups_per_flex;
        from = group - group % size;
        to = from + size < superblock.group_count ? from + size : superblock.group_count;
    }
    for (std::uint64_t other = from; other < to; ++other)
    {
        const GroupDescriptor descriptor = Group(static_cast<std::uint32_t>(other));
        if (block == descriptor.block_bitmap || block == descriptor.inode_bitmap ||
            (block >= descriptor.inode_table &&
             block < descriptor.inode_table + inode_table_blocks))
        {
            return true;
        }
    }
    return false;
}

// The block bitmap of a group that never had one written, as Linux's ext4_init_block_bitmap makes
// it: what lies in the group of the superblock and descriptor copies and of any group's bitmaps
// and inode table is in use, the rest free.
void Ext2Filesystem::MarkGroupMetadata(std::uint32_t group, std::span<std::uint8_t> bitmap) const
{
    const std::uint64_t first = GroupFirstBlock(group);
    const std::uint64_t end = first + BlocksInGroup(group);
    const auto mark = [&](std::uint64_t start, std::uint64_t length)
    {
        for (std::uint64_t block = start > first ? start : first;
             block < start + length && block < end; ++block)
        {
            SetBit(bitmap, static_cast<std::uint32_t>(block - first));
        }
    };
    if (GroupHasSuperblock(superblock, group))
    {
        mark(first, 1 + descriptor_blocks + superblock.reserved_descriptor_blocks);
    }
    for (std::uint32_t other = 0; other < superblock.group_count; ++other)
    {
        const GroupDescriptor descriptor = Group(other);
        mark(descriptor.block_bitmap, 1);
        mark(descriptor.inode_bitmap, 1);
        mark(descriptor.inode_table, inode_table_blocks);
    }
}

std::uint32_t Ext2Filesystem::BitmapChecksumOf(std::span<const std::uint8_t> bitmap,
                                               std::uint32_t bits) const
{
    const std::uint32_t checksum = BitmapChecksum(volume_seed, bitmap.first(bits / 8));
    return descriptor_size >= LargeGroupDescriptorSize ? checksum : checksum & 0xFFFFU;
}

// Linux marks the group's bitmap corrupt and stops using it; here the whole mount stops writing.
std::unexpected<Error> Ext2Filesystem::BitmapMismatch(std::string_view message, std::uint32_t group)
{
    LogWithValue(message, group);
    read_only = true;
    return std::unexpected(Error::Corrupt);
}

// The bits past the group's last block or inode are set, as mke2fs and Linux leave them.
void Ext2Filesystem::SetPadding(std::span<std::uint8_t> bitmap, std::uint32_t used_bits) const
{
    for (std::uint32_t bit = used_bits; bit < block_size * 8; ++bit)
    {
        SetBit(bitmap, bit);
    }
}

Status Ext2Filesystem::LoadBlockBitmap(std::uint32_t group, std::span<std::uint8_t> bitmap)
{
    const GroupDescriptor descriptor = Group(group);
    const std::uint32_t count = BlocksInGroup(group);
    if (group_checksums && (descriptor.flags & GroupBlocksUnused) != 0)
    {
        ZeroBytes(bitmap);
        MarkGroupMetadata(group, bitmap);
        SetPadding(bitmap, count);
        std::uint32_t free = 0;
        for (std::uint32_t bit = 0; bit < count; ++bit)
        {
            free += TestBit(bitmap, bit) ? 0 : 1;
        }
        return free == descriptor.free_blocks ? Status{} : std::unexpected(Error::Corrupt);
    }
    if (!IsDataBlock(descriptor.block_bitmap))
    {
        return std::unexpected(Error::Corrupt);
    }
    if (const Status read = ReadMetadata(descriptor.block_bitmap, 0, bitmap, bitmap); !read)
    {
        return read;
    }
    if (metadata_checksums &&
        BitmapChecksumOf(bitmap, superblock.blocks_per_group) != descriptor.block_bitmap_checksum)
    {
        return BitmapMismatch("ext2: mounted read-only: block bitmap checksum mismatch in group ",
                              group);
    }
    return {};
}

Status Ext2Filesystem::LoadInodeBitmap(std::uint32_t group, std::span<std::uint8_t> bitmap)
{
    const GroupDescriptor descriptor = Group(group);
    if (group_checksums && (descriptor.flags & GroupInodesUnused) != 0)
    {
        ZeroBytes(bitmap);
        SetPadding(bitmap, superblock.inodes_per_group);
        return descriptor.free_inodes == superblock.inodes_per_group
                   ? Status{}
                   : std::unexpected(Error::Corrupt);
    }
    if (!IsDataBlock(descriptor.inode_bitmap))
    {
        return std::unexpected(Error::Corrupt);
    }
    if (const Status read = ReadMetadata(descriptor.inode_bitmap, 0, bitmap, bitmap); !read)
    {
        return read;
    }
    if (metadata_checksums &&
        BitmapChecksumOf(bitmap, superblock.inodes_per_group) != descriptor.inode_bitmap_checksum)
    {
        return BitmapMismatch("ext2: mounted read-only: inode bitmap checksum mismatch in group ",
                              group);
    }
    return {};
}

// descriptor holds the group's new counts and flags. A cleared uninit flag or a lowered
// bg_itable_unused goes to the disk at once, before anything relies on it.
Status Ext2Filesystem::CommitBitmap(std::uint32_t group, GroupDescriptor descriptor, bool blocks,
                                    std::span<const std::uint8_t> bitmap)
{
    const GroupDescriptor before = Group(group);
    const std::uint64_t place = blocks ? descriptor.block_bitmap : descriptor.inode_bitmap;
    if (const Status written = WriteMetadata(place, bitmap); !written)
    {
        return written;
    }
    if (metadata_checksums)
    {
        if (blocks)
        {
            descriptor.block_bitmap_checksum =
                BitmapChecksumOf(bitmap, superblock.blocks_per_group);
        }
        else
        {
            descriptor.inode_bitmap_checksum =
                BitmapChecksumOf(bitmap, superblock.inodes_per_group);
        }
    }
    StoreGroup(group, descriptor);
    if (descriptor.flags != before.flags || descriptor.unused_inodes != before.unused_inodes)
    {
        return WriteGroups();
    }
    return {};
}

std::optional<std::string_view> Ext2Filesystem::CheckGroups() const
{
    std::uint64_t free_blocks = 0;
    std::uint64_t free_inodes = 0;
    for (std::uint32_t group = 0; group < superblock.group_count; ++group)
    {
        const GroupDescriptor descriptor = Group(group);
        if (!IsDataBlock(descriptor.block_bitmap) || !IsDataBlock(descriptor.inode_bitmap) ||
            !IsDataBlock(descriptor.inode_table) ||
            descriptor.inode_table + inode_table_blocks > superblock.blocks_count ||
            descriptor.free_blocks > BlocksInGroup(group) ||
            descriptor.free_inodes > superblock.inodes_per_group ||
            descriptor.used_directories > superblock.inodes_per_group)
        {
            return "a group descriptor is inconsistent";
        }
        // e2fsck's own checks of what group checksums let a descriptor leave unwritten.
        if (group_checksums &&
            (descriptor.unused_inodes > descriptor.free_inodes ||
             ((descriptor.flags & GroupInodesUnused) != 0 &&
              descriptor.free_inodes != superblock.inodes_per_group) ||
             ((descriptor.flags & GroupBlocksUnused) != 0 && group + 1 == superblock.group_count)))
        {
            return "a group descriptor's unused parts are inconsistent";
        }
        free_blocks += descriptor.free_blocks;
        free_inodes += descriptor.free_inodes;
    }
    if (free_blocks > superblock.blocks_count || free_inodes > superblock.inodes_count)
    {
        return "the group descriptors count more than the volume holds";
    }
    return std::nullopt;
}

Status Ext2Filesystem::WriteGroups()
{
    if (dirty_descriptors_first == dirty_descriptors_end)
    {
        return {};
    }
    const std::uint64_t first = dirty_descriptors_first;
    const std::uint64_t count = dirty_descriptors_end - first;
    if (const Status written =
            WriteMetadata(superblock.first_data_block + 1 + first,
                          descriptors.subspan(static_cast<std::size_t>(first * block_size),
                                              static_cast<std::size_t>(count * block_size)));
        !written)
    {
        return written;
    }
    dirty_descriptors_first = 0;
    dirty_descriptors_end = 0;
    return {};
}

// The reserved blocks (s_r_blocks_count) are for the superuser, and FeatherOS has no users: every
// caller counts as root, so the search may take every free block. Block maps hold 32-bit numbers,
// so blocks from 2^32 on are never handed out.
Result<std::uint32_t> Ext2Filesystem::AllocateBlock(std::uint32_t goal,
                                                    std::span<std::uint8_t> bitmap)
{
    const Result<AllocatedRun> run = AllocateBlocks(goal, 1, bitmap);
    if (!run)
    {
        return Failure(run);
    }
    return run->first;
}

// The run continues only within the group of its first block, over free bits that are no group
// metadata, so it costs one bitmap write.
Result<AllocatedRun> Ext2Filesystem::AllocateBlocks(std::uint32_t goal, std::uint32_t wanted,
                                                    std::span<std::uint8_t> bitmap)
{
    if (superblock.free_blocks_count == 0)
    {
        return std::unexpected(Error::NoSpace);
    }
    if (goal <= superblock.first_data_block || goal >= superblock.blocks_count)
    {
        goal = superblock.first_data_block + 1;
    }
    const std::uint32_t goal_group =
        (goal - superblock.first_data_block) / superblock.blocks_per_group;
    const std::uint32_t goal_bit =
        (goal - superblock.first_data_block) % superblock.blocks_per_group;
    constexpr std::uint64_t MappableEnd = std::uint64_t{1} << 32;
    for (std::uint32_t step = 0; step < superblock.group_count; ++step)
    {
        const std::uint32_t group = (goal_group + step) % superblock.group_count;
        GroupDescriptor descriptor = Group(group);
        const std::uint64_t group_first = GroupFirstBlock(group);
        if (descriptor.free_blocks == 0 || group_first >= MappableEnd)
        {
            continue;
        }
        if (const Status loaded = LoadBlockBitmap(group, bitmap); !loaded)
        {
            return Failure(loaded);
        }
        // A block freed in a transaction that has not committed stays taken as far as the
        // committed state goes, so that a replay never lets the old copy of its contents
        // overwrite what the new owner writes: the search skips what either bitmap marks.
        const bool frozen = GroupFrozen(group);
        std::span<std::uint8_t> search = bitmap;
        Buffers committed_view(block_size, frozen ? 3 : 0);
        if (frozen)
        {
            if (committed_view.Empty())
            {
                return std::unexpected(Error::OutOfMemory);
            }
            if (const Status loaded =
                    LoadCommittedBlockBitmap(group, committed_view[0], committed_view[1]);
                !loaded)
            {
                return Failure(loaded);
            }
            search = committed_view[2];
            for (std::size_t index = 0; index < block_size; ++index)
            {
                search[index] = bitmap[index] | committed_view[0][index];
            }
        }
        std::uint32_t count = BlocksInGroup(group);
        if (group_first + count > MappableEnd)
        {
            count = static_cast<std::uint32_t>(MappableEnd - group_first);
        }
        const std::uint32_t start = step == 0 && goal_bit < count ? goal_bit : 0;
        std::optional<std::uint32_t> bit = FindClearBit(search, start, count);
        if (!bit && start > 0)
        {
            bit = FindClearBit(search, 0, start);
        }
        if (!bit && (count < BlocksInGroup(group) || frozen))
        {
            continue;
        }
        // The descriptor counts free blocks the bitmap does not have, or the bitmap offers one
        // of the group's own structures.
        const std::uint64_t block = group_first + (bit ? *bit : 0);
        if (!bit || !IsDataBlock(block) || IsGroupMetadata(group, block))
        {
            return std::unexpected(Error::Corrupt);
        }
        std::uint32_t taken = 0;
        while (taken < wanted && *bit + taken < count && taken < descriptor.free_blocks &&
               taken < superblock.free_blocks_count && !TestBit(search, *bit + taken) &&
               IsDataBlock(block + taken) && !IsGroupMetadata(group, block + taken))
        {
            SetBit(bitmap, *bit + taken);
            ++taken;
        }
        descriptor.free_blocks -= taken;
        descriptor.flags = static_cast<std::uint16_t>(descriptor.flags & ~GroupBlocksUnused);
        if (const Status written = CommitBitmap(group, descriptor, true, bitmap); !written)
        {
            return Failure(written);
        }
        superblock.free_blocks_count -= taken;
        return AllocatedRun{.first = static_cast<std::uint32_t>(block), .count = taken};
    }
    return std::unexpected(Error::NoSpace);
}

// Orlov-lite, after Linux's ext2: a directory in the root starts a subtree of its own, so it goes
// to the group with the fewest directories among those with at least the average share of free
// inodes and blocks, which spreads the top of the tree over the volume. Any other directory starts
// looking in its parent's group, as files do, so a subtree stays together.
std::uint32_t Ext2Filesystem::DirectoryGroup(std::uint32_t parent_group, bool top_level) const
{
    if (!top_level)
    {
        return parent_group;
    }
    const std::uint32_t average_inodes = superblock.free_inodes_count / superblock.group_count;
    const std::uint64_t average_blocks = superblock.free_blocks_count / superblock.group_count;
    std::optional<std::uint32_t> best;
    std::uint32_t fewest = 0;
    for (std::uint32_t group = 0; group < superblock.group_count; ++group)
    {
        const GroupDescriptor descriptor = Group(group);
        if (descriptor.free_inodes == 0 || descriptor.free_blocks == 0 ||
            descriptor.free_inodes < average_inodes || descriptor.free_blocks < average_blocks)
        {
            continue;
        }
        if (!best || descriptor.used_directories < fewest)
        {
            best = group;
            fewest = descriptor.used_directories;
        }
    }
    return best ? *best : parent_group;
}

Result<NodeId> Ext2Filesystem::AllocateInode(std::uint32_t parent_group, bool directory,
                                             std::span<std::uint8_t> bitmap,
                                             std::span<std::uint8_t> scratch)
{
    if (superblock.free_inodes_count == 0)
    {
        return std::unexpected(Error::NoSpace);
    }
    // A group with free blocks too keeps the file's data near its inode; failing that, any group
    // with an inode.
    std::optional<std::uint32_t> chosen;
    for (int pass = 0; pass < 2; ++pass)
    {
        const bool needs_blocks = pass == 0;
        for (std::uint32_t step = 0; step < superblock.group_count && !chosen; ++step)
        {
            const std::uint32_t group = (parent_group + step) % superblock.group_count;
            const GroupDescriptor descriptor = Group(group);
            if (descriptor.free_inodes > 0 && (!needs_blocks || descriptor.free_blocks > 0))
            {
                chosen = group;
            }
        }
    }
    if (!chosen)
    {
        return std::unexpected(Error::NoSpace);
    }
    const std::uint32_t group = *chosen;
    // As Linux's __ext4_new_inode: a group with inodes in use has its block bitmap written.
    if (group_checksums && (Group(group).flags & GroupBlocksUnused) != 0)
    {
        if (const Status loaded = LoadBlockBitmap(group, bitmap); !loaded)
        {
            return Failure(loaded);
        }
        GroupDescriptor blocks = Group(group);
        blocks.flags = static_cast<std::uint16_t>(blocks.flags & ~GroupBlocksUnused);
        if (const Status written = CommitBitmap(group, blocks, true, bitmap); !written)
        {
            return Failure(written);
        }
    }
    GroupDescriptor descriptor = Group(group);
    if (const Status loaded = LoadInodeBitmap(group, bitmap); !loaded)
    {
        return Failure(loaded);
    }
    // Inodes below s_first_ino are reserved (the root, the journal, the resize inode, ...).
    const std::uint64_t group_first = std::uint64_t{group} * superblock.inodes_per_group + 1;
    const std::uint32_t start =
        superblock.first_inode > group_first
            ? static_cast<std::uint32_t>(superblock.first_inode - group_first)
            : 0;
    const std::optional<std::uint32_t> bit =
        FindClearBit(bitmap, start, superblock.inodes_per_group);
    if (!bit)
    {
        return std::unexpected(Error::Corrupt);
    }
    const NodeId node = group_first + *bit;
    // A free inode keeps no links; one that does is in use whatever its bitmap says.
    const Result<Inode> inode = LoadInode(node, scratch);
    if (!inode)
    {
        return Failure(inode);
    }
    if (inode->link_count != 0)
    {
        return std::unexpected(Error::Corrupt);
    }
    SetBit(bitmap, *bit);
    --descriptor.free_inodes;
    if (directory)
    {
        ++descriptor.used_directories;
    }
    if (group_checksums)
    {
        // As Linux's __ext4_new_inode: the table is in use up to the new inode.
        std::uint32_t used = superblock.inodes_per_group - descriptor.unused_inodes;
        if ((descriptor.flags & GroupInodesUnused) != 0)
        {
            descriptor.flags = static_cast<std::uint16_t>(descriptor.flags & ~GroupInodesUnused);
            used = 0;
        }
        if (*bit + 1 > used)
        {
            descriptor.unused_inodes = superblock.inodes_per_group - (*bit + 1);
        }
    }
    if (const Status written = CommitBitmap(group, descriptor, false, bitmap); !written)
    {
        return Failure(written);
    }
    --superblock.free_inodes_count;
    return node;
}

Status Ext2Filesystem::FreeInode(NodeId node, bool directory, std::span<std::uint8_t> bitmap)
{
    const std::uint32_t group = GroupOfInode(node);
    const auto bit = static_cast<std::uint32_t>((node - 1) % superblock.inodes_per_group);
    GroupDescriptor descriptor = Group(group);
    if (const Status loaded = LoadInodeBitmap(group, bitmap); !loaded)
    {
        return loaded;
    }
    if (!TestBit(bitmap, bit) || descriptor.free_inodes >= superblock.inodes_per_group ||
        (directory && descriptor.used_directories == 0))
    {
        return std::unexpected(Error::Corrupt);
    }
    ClearBit(bitmap, bit);
    ++descriptor.free_inodes;
    if (directory)
    {
        --descriptor.used_directories;
    }
    if (const Status written = CommitBitmap(group, descriptor, false, bitmap); !written)
    {
        return written;
    }
    ++superblock.free_inodes_count;
    return {};
}

// Freeing happens after nothing points to the block any more, so the bitmap may wait for the
// next group's block or FinishFrees.
Status Ext2Filesystem::FreeBlock(FreeCursor& cursor, std::uint64_t block)
{
    if (!IsDataBlock(block))
    {
        return std::unexpected(Error::Corrupt);
    }
    const auto group = static_cast<std::uint32_t>((block - superblock.first_data_block) /
                                                  superblock.blocks_per_group);
    const auto bit = static_cast<std::uint32_t>((block - superblock.first_data_block) %
                                                superblock.blocks_per_group);
    if (IsGroupMetadata(group, block))
    {
        return std::unexpected(Error::Corrupt);
    }
    if (!cursor.loaded || cursor.group != group)
    {
        if (const Status finished = FinishFrees(cursor); !finished)
        {
            return finished;
        }
        if (const Status loaded = LoadBlockBitmap(group, cursor.bitmap); !loaded)
        {
            return loaded;
        }
        cursor.group = group;
        cursor.loaded = true;
    }
    GroupDescriptor descriptor = Group(group);
    // Freeing a free block would count it twice.
    if (!TestBit(cursor.bitmap, bit) || descriptor.free_blocks >= BlocksInGroup(group))
    {
        return std::unexpected(Error::Corrupt);
    }
    if (const Status noted = NoteFreedBlock(group, block); !noted)
    {
        return noted;
    }
    ClearBit(cursor.bitmap, bit);
    cursor.dirty = true;
    ++descriptor.free_blocks;
    StoreGroup(group, descriptor);
    ++superblock.free_blocks_count;
    return {};
}

Status Ext2Filesystem::FreeBlocks(FreeCursor& cursor, std::uint64_t first, std::uint64_t count)
{
    for (std::uint64_t block = first; block < first + count; ++block)
    {
        if (const Status freed = FreeBlock(cursor, block); !freed)
        {
            return freed;
        }
    }
    return {};
}

Status Ext2Filesystem::FinishFrees(FreeCursor& cursor)
{
    // The next free reads the bitmap again: allocations may change it in between.
    cursor.loaded = false;
    if (!cursor.dirty)
    {
        return {};
    }
    cursor.dirty = false;
    return CommitBitmap(cursor.group, Group(cursor.group), true, cursor.bitmap);
}

} // namespace FeatherDisk::Ext
