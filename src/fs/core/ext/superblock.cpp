// SPDX-License-Identifier: MIT OR Apache-2.0
#include "ext/superblock.h"

#include "common/little_endian.h"

#include <bit>

namespace FeatherDisk::Ext
{
namespace
{

// Larger block sizes exist on machines with bigger pages; 64 KiB is the format's limit.
constexpr std::uint32_t MaximumLogBlockSize = 6;

std::uint16_t Load16(const std::uint8_t* bytes)
{
    return LoadLittleEndian<std::uint16_t>(bytes);
}

std::uint32_t Load32(const std::uint8_t* bytes)
{
    return LoadLittleEndian<std::uint32_t>(bytes);
}

std::uint64_t DivideRoundingUp(std::uint64_t value, std::uint64_t divisor)
{
    return value / divisor + (value % divisor != 0 ? 1 : 0);
}

} // namespace

Superblock DecodeSuperblock(std::span<const std::uint8_t, SuperblockSize> bytes)
{
    const std::uint8_t* raw = bytes.data();
    Superblock superblock = {
        .inodes_count = Load32(raw + 0),
        .blocks_count = Load32(raw + 4),
        .free_blocks_count = Load32(raw + 12),
        .free_inodes_count = Load32(raw + 16),
        .first_data_block = Load32(raw + 20),
        .log_block_size = Load32(raw + 24),
        .block_size = 0,
        .blocks_per_group = Load32(raw + 32),
        .inodes_per_group = Load32(raw + 40),
        .magic = Load16(raw + 56),
        .revision = Load32(raw + 76),
        .inode_size = GoodOldInodeSize,
        .compatible = 0,
        .incompatible = 0,
        .read_only_compatible = 0,
        .group_count = 0,
    };
    if (superblock.log_block_size <= MaximumLogBlockSize)
    {
        superblock.block_size = MinimumBlockSize << superblock.log_block_size;
    }
    // Revision 0 has none of the fields from offset 84 on; they read as zero there.
    if (superblock.revision >= DynamicRevision)
    {
        superblock.inode_size = Load16(raw + 88);
        superblock.compatible = Load32(raw + 92);
        superblock.incompatible = Load32(raw + 96);
        superblock.read_only_compatible = Load32(raw + 100);
    }
    if (superblock.blocks_per_group != 0 && superblock.blocks_count > superblock.first_data_block)
    {
        superblock.group_count = static_cast<std::uint32_t>(DivideRoundingUp(
            superblock.blocks_count - superblock.first_data_block, superblock.blocks_per_group));
    }
    return superblock;
}

bool LooksLikeExt(const Superblock& superblock)
{
    return superblock.magic == Magic && superblock.block_size != 0 &&
           superblock.inodes_count != 0 && superblock.blocks_per_group != 0 &&
           superblock.inodes_per_group != 0 && superblock.group_count != 0;
}

Status CheckSuperblock(const Superblock& superblock, std::uint64_t volume_bytes)
{
    if (!LooksLikeExt(superblock))
    {
        return std::unexpected(Error::Corrupt);
    }
    if (superblock.revision > DynamicRevision || superblock.block_size > MaximumBlockSize)
    {
        return std::unexpected(Error::Unsupported);
    }
    const std::uint32_t block_size = superblock.block_size;
    const std::uint32_t inode_size = superblock.inode_size;
    const std::uint32_t bits_per_block = block_size * 8;
    // The superblock lives in block 1 of a 1 KiB-block volume and in block 0 otherwise.
    const std::uint32_t first_data_block = block_size == MinimumBlockSize ? 1 : 0;
    if (inode_size < GoodOldInodeSize || inode_size > block_size ||
        !std::has_single_bit(inode_size) || superblock.first_data_block != first_data_block ||
        superblock.blocks_per_group > bits_per_block ||
        superblock.inodes_per_group > bits_per_block)
    {
        return std::unexpected(Error::Corrupt);
    }
    const std::uint64_t group_count = superblock.group_count;
    if (std::uint64_t{superblock.inodes_count} != group_count * superblock.inodes_per_group)
    {
        return std::unexpected(Error::Corrupt);
    }
    // The group descriptor table follows the superblock's block.
    const std::uint64_t table_blocks =
        DivideRoundingUp(group_count * GroupDescriptorSize, block_size);
    if (std::uint64_t{first_data_block} + 1 + table_blocks > superblock.blocks_count ||
        std::uint64_t{superblock.blocks_count} * block_size > volume_bytes)
    {
        return std::unexpected(Error::Corrupt);
    }
    return {};
}

} // namespace FeatherDisk::Ext
