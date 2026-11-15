// SPDX-License-Identifier: MIT OR Apache-2.0
// Block groups and their bitmaps. A volume is cut into groups of blocks_per_group blocks and
// inodes_per_group inodes; each group has one block of bitmap for its blocks and one for its
// inodes, and its descriptor counts what is free. The descriptors stay in memory and go back to
// the volume on Sync; a bitmap is written as soon as it changes, so a block or inode is marked
// before anything points to it.
#include "common/bitmap.h"
#include "ext/ext2_filesystem.h"

namespace FeatherDisk::Ext
{

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
        descriptors.subspan(std::size_t{group} * GroupDescriptorSize).first<GroupDescriptorSize>());
}

void Ext2Filesystem::StoreGroup(std::uint32_t group, const GroupDescriptor& descriptor)
{
    const std::size_t offset = std::size_t{group} * GroupDescriptorSize;
    EncodeGroupCounts(descriptor, descriptors.subspan(offset).first<GroupDescriptorSize>());
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

// The bitmaps, the inode table and, in a group with a backup, the copies of the superblock, the
// descriptor table and the blocks reserved for its growth. Only data may be allocated or freed.
bool Ext2Filesystem::IsGroupMetadata(std::uint32_t group, std::uint64_t block) const
{
    const GroupDescriptor descriptor = Group(group);
    if (block == descriptor.block_bitmap || block == descriptor.inode_bitmap ||
        (block >= descriptor.inode_table && block < descriptor.inode_table + inode_table_blocks))
    {
        return true;
    }
    const std::uint64_t first = GroupFirstBlock(group);
    return GroupHasSuperblock(superblock, group) && block >= first &&
           block < first + 1 + descriptor_blocks + superblock.reserved_descriptor_blocks;
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
            cache.Write(superblock.first_data_block + 1 + first,
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
// caller counts as root, so the search may take every free block.
Result<std::uint32_t> Ext2Filesystem::AllocateBlock(std::uint32_t goal,
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
    for (std::uint32_t step = 0; step < superblock.group_count; ++step)
    {
        const std::uint32_t group = (goal_group + step) % superblock.group_count;
        GroupDescriptor descriptor = Group(group);
        if (descriptor.free_blocks == 0)
        {
            continue;
        }
        if (!IsDataBlock(descriptor.block_bitmap))
        {
            return std::unexpected(Error::Corrupt);
        }
        if (const Status read = cache.Read(descriptor.block_bitmap, 0, bitmap, bitmap); !read)
        {
            return Failure(read);
        }
        const std::uint32_t count = BlocksInGroup(group);
        const std::uint32_t start = step == 0 && goal_bit < count ? goal_bit : 0;
        std::optional<std::uint32_t> bit = FindClearBit(bitmap, start, count);
        if (!bit && start > 0)
        {
            bit = FindClearBit(bitmap, 0, start);
        }
        // The descriptor counts free blocks the bitmap does not have, or the bitmap offers one
        // of the group's own structures.
        const std::uint64_t block = GroupFirstBlock(group) + (bit ? *bit : 0);
        if (!bit || !IsDataBlock(block) || IsGroupMetadata(group, block))
        {
            return std::unexpected(Error::Corrupt);
        }
        SetBit(bitmap, *bit);
        if (const Status written = cache.Write(descriptor.block_bitmap, bitmap); !written)
        {
            return Failure(written);
        }
        --descriptor.free_blocks;
        StoreGroup(group, descriptor);
        --superblock.free_blocks_count;
        return static_cast<std::uint32_t>(block);
    }
    return std::unexpected(Error::NoSpace);
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
    GroupDescriptor descriptor = Group(group);
    if (!IsDataBlock(descriptor.inode_bitmap))
    {
        return std::unexpected(Error::Corrupt);
    }
    if (const Status read = cache.Read(descriptor.inode_bitmap, 0, bitmap, bitmap); !read)
    {
        return Failure(read);
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
    if (const Status written = cache.Write(descriptor.inode_bitmap, bitmap); !written)
    {
        return Failure(written);
    }
    --descriptor.free_inodes;
    if (directory)
    {
        ++descriptor.used_directories;
    }
    StoreGroup(group, descriptor);
    --superblock.free_inodes_count;
    return node;
}

Status Ext2Filesystem::FreeInode(NodeId node, bool directory, std::span<std::uint8_t> bitmap)
{
    const std::uint32_t group = GroupOfInode(node);
    const auto bit = static_cast<std::uint32_t>((node - 1) % superblock.inodes_per_group);
    GroupDescriptor descriptor = Group(group);
    if (!IsDataBlock(descriptor.inode_bitmap))
    {
        return std::unexpected(Error::Corrupt);
    }
    if (const Status read = cache.Read(descriptor.inode_bitmap, 0, bitmap, bitmap); !read)
    {
        return read;
    }
    if (!TestBit(bitmap, bit) || descriptor.free_inodes >= superblock.inodes_per_group ||
        (directory && descriptor.used_directories == 0))
    {
        return std::unexpected(Error::Corrupt);
    }
    ClearBit(bitmap, bit);
    if (const Status written = cache.Write(descriptor.inode_bitmap, bitmap); !written)
    {
        return written;
    }
    ++descriptor.free_inodes;
    if (directory)
    {
        --descriptor.used_directories;
    }
    StoreGroup(group, descriptor);
    ++superblock.free_inodes_count;
    return {};
}

// Freeing happens after nothing points to the block any more, so the bitmap may wait for the
// next group's block or FinishFrees.
Status Ext2Filesystem::FreeBlock(FreeCursor& cursor, std::uint32_t block)
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
        const GroupDescriptor descriptor = Group(group);
        if (!IsDataBlock(descriptor.block_bitmap))
        {
            return std::unexpected(Error::Corrupt);
        }
        if (const Status read =
                cache.Read(descriptor.block_bitmap, 0, cursor.bitmap, cursor.bitmap);
            !read)
        {
            return read;
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
    ClearBit(cursor.bitmap, bit);
    cursor.dirty = true;
    ++descriptor.free_blocks;
    StoreGroup(group, descriptor);
    ++superblock.free_blocks_count;
    return {};
}

Status Ext2Filesystem::FinishFrees(FreeCursor& cursor)
{
    if (!cursor.dirty)
    {
        return {};
    }
    cursor.dirty = false;
    return cache.Write(Group(cursor.group).block_bitmap, cursor.bitmap);
}

} // namespace FeatherDisk::Ext
