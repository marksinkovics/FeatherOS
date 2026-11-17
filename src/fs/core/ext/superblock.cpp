// SPDX-License-Identifier: MIT OR Apache-2.0
#include "ext/superblock.h"

#include "common/crc.h"
#include "common/little_endian.h"

#include <bit>

namespace FeatherDisk::Ext
{
namespace
{

// Larger block sizes exist on machines with bigger pages; 64 KiB is the format's limit.
constexpr std::uint32_t MaximumLogBlockSize = 6;
// s_checksum covers the superblock up to itself.
constexpr std::size_t SuperblockChecksumOffset = 1020;

std::uint16_t Load16(const std::uint8_t* bytes)
{
    return LoadLittleEndian<std::uint16_t>(bytes);
}

std::uint32_t Load32(const std::uint8_t* bytes)
{
    return LoadLittleEndian<std::uint32_t>(bytes);
}

bool IsPowerOf(std::uint32_t value, std::uint32_t base)
{
    while (value > 1 && value % base == 0)
    {
        value /= base;
    }
    return value == 1;
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
        .reserved_blocks_count = Load32(raw + 8),
        .mount_time = Load32(raw + 44),
        .write_time = Load32(raw + 48),
        .mount_count = Load16(raw + 52),
        .state = Load16(raw + 58),
        .errors = Load16(raw + 60),
        .first_inode = GoodOldFirstInode,
        .reserved_descriptor_blocks = 0,
        .minimum_extra_size = 0,
        .wanted_extra_size = 0,
        .descriptor_size = GroupDescriptorSize,
        .uuid = {},
        .checksum_type = 0,
        .stored_checksum_seed = 0,
        .backup_groups = {},
        .checksum_matches = true,
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
        superblock.first_inode = Load32(raw + 84);
        if ((superblock.compatible & CompatibleResizeInode) != 0)
        {
            superblock.reserved_descriptor_blocks = Load16(raw + 206);
        }
        if (superblock.inode_size > GoodOldInodeSize)
        {
            superblock.minimum_extra_size = Load16(raw + 348);
            superblock.wanted_extra_size = Load16(raw + 350);
        }
        if ((superblock.incompatible & Incompatible64Bit) != 0)
        {
            superblock.blocks_count |= std::uint64_t{Load32(raw + 336)} << 32;
            superblock.reserved_blocks_count |= std::uint64_t{Load32(raw + 340)} << 32;
            superblock.free_blocks_count |= std::uint64_t{Load32(raw + 344)} << 32;
            superblock.descriptor_size = Load16(raw + 254);
        }
        for (std::size_t index = 0; index < superblock.uuid.size(); ++index)
        {
            superblock.uuid[index] = raw[104 + index];
        }
        superblock.checksum_type = raw[373];
        superblock.stored_checksum_seed = Load32(raw + 624);
        superblock.backup_groups = {Load32(raw + 588), Load32(raw + 592)};
        if (HasMetadataChecksums(superblock))
        {
            superblock.checksum_matches = Crc32c(~0U, bytes.first(SuperblockChecksumOffset)) ==
                                          Load32(raw + SuperblockChecksumOffset);
        }
    }
    if (superblock.blocks_per_group != 0 && superblock.blocks_count > superblock.first_data_block)
    {
        const std::uint64_t groups = DivideRoundingUp(
            superblock.blocks_count - superblock.first_data_block, superblock.blocks_per_group);
        superblock.group_count =
            groups <= ~std::uint32_t{0} ? static_cast<std::uint32_t>(groups) : 0;
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
    if (HasMetadataChecksums(superblock))
    {
        if (superblock.checksum_type != ChecksumTypeCrc32c)
        {
            return std::unexpected(Error::Unsupported);
        }
        if (!superblock.checksum_matches)
        {
            return std::unexpected(Error::Corrupt);
        }
    }
    const std::uint32_t block_size = superblock.block_size;
    const std::uint32_t inode_size = superblock.inode_size;
    const std::uint32_t bits_per_block = block_size * 8;
    // The superblock lives in block 1 of a 1 KiB-block volume and in block 0 otherwise.
    const std::uint32_t first_data_block = block_size == MinimumBlockSize ? 1 : 0;
    if (inode_size < GoodOldInodeSize || inode_size > block_size ||
        !std::has_single_bit(inode_size) || superblock.first_data_block != first_data_block ||
        superblock.blocks_per_group > bits_per_block ||
        superblock.inodes_per_group > bits_per_block ||
        superblock.descriptor_size < GroupDescriptorSize ||
        superblock.descriptor_size > MaximumGroupDescriptorSize ||
        !std::has_single_bit(superblock.descriptor_size) ||
        ((superblock.incompatible & Incompatible64Bit) != 0 &&
         superblock.descriptor_size < LargeGroupDescriptorSize))
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
        DivideRoundingUp(group_count * superblock.descriptor_size, block_size);
    if (std::uint64_t{first_data_block} + 1 + table_blocks > superblock.blocks_count ||
        superblock.blocks_count > volume_bytes / block_size)
    {
        return std::unexpected(Error::Corrupt);
    }
    return {};
}

void EncodeSuperblockState(const Superblock& superblock,
                           std::span<std::uint8_t, SuperblockSize> bytes)
{
    std::uint8_t* raw = bytes.data();
    StoreLittleEndian(raw + 12, static_cast<std::uint32_t>(superblock.free_blocks_count));
    StoreLittleEndian(raw + 16, superblock.free_inodes_count);
    StoreLittleEndian(raw + 44, superblock.mount_time);
    StoreLittleEndian(raw + 48, superblock.write_time);
    StoreLittleEndian(raw + 52, superblock.mount_count);
    StoreLittleEndian(raw + 58, superblock.state);
    if (superblock.revision >= DynamicRevision)
    {
        StoreLittleEndian(raw + 100, superblock.read_only_compatible);
    }
    if ((superblock.incompatible & Incompatible64Bit) != 0)
    {
        StoreLittleEndian(raw + 344,
                          static_cast<std::uint32_t>(superblock.free_blocks_count >> 32));
    }
}

bool GroupHasSuperblock(const Superblock& superblock, std::uint32_t group)
{
    if (group == 0)
    {
        return true;
    }
    if ((superblock.compatible & CompatibleSparseSuper2) != 0)
    {
        return group == superblock.backup_groups[0] || group == superblock.backup_groups[1];
    }
    if (group == 1 || (superblock.read_only_compatible & ReadOnlyCompatibleSparseSuper) == 0)
    {
        return true;
    }
    return IsPowerOf(group, 3) || IsPowerOf(group, 5) || IsPowerOf(group, 7);
}

bool HasGroupChecksums(const Superblock& superblock)
{
    return (superblock.read_only_compatible &
            (ReadOnlyCompatibleGroupChecksum | ReadOnlyCompatibleMetadataChecksum)) != 0;
}

bool HasMetadataChecksums(const Superblock& superblock)
{
    return (superblock.read_only_compatible & ReadOnlyCompatibleMetadataChecksum) != 0;
}

GroupDescriptor DecodeGroupDescriptor(std::span<const std::uint8_t> bytes)
{
    const std::uint8_t* raw = bytes.data();
    GroupDescriptor descriptor = {
        .block_bitmap = Load32(raw + 0),
        .inode_bitmap = Load32(raw + 4),
        .inode_table = Load32(raw + 8),
        .free_blocks = Load16(raw + 12),
        .free_inodes = Load16(raw + 14),
        .used_directories = Load16(raw + 16),
        .flags = Load16(raw + 18),
        .unused_inodes = Load16(raw + 28),
        .block_bitmap_checksum = Load16(raw + 24),
        .inode_bitmap_checksum = Load16(raw + 26),
        .checksum = Load16(raw + 30),
    };
    if (bytes.size() >= LargeGroupDescriptorSize)
    {
        descriptor.block_bitmap |= std::uint64_t{Load32(raw + 32)} << 32;
        descriptor.inode_bitmap |= std::uint64_t{Load32(raw + 36)} << 32;
        descriptor.inode_table |= std::uint64_t{Load32(raw + 40)} << 32;
        descriptor.free_blocks |= std::uint32_t{Load16(raw + 44)} << 16;
        descriptor.free_inodes |= std::uint32_t{Load16(raw + 46)} << 16;
        descriptor.used_directories |= std::uint32_t{Load16(raw + 48)} << 16;
        descriptor.unused_inodes |= std::uint32_t{Load16(raw + 50)} << 16;
        descriptor.block_bitmap_checksum |= std::uint32_t{Load16(raw + 56)} << 16;
        descriptor.inode_bitmap_checksum |= std::uint32_t{Load16(raw + 58)} << 16;
    }
    return descriptor;
}

void EncodeGroupCounts(const GroupDescriptor& descriptor, std::span<std::uint8_t> bytes)
{
    std::uint8_t* raw = bytes.data();
    StoreLittleEndian(raw + 12, static_cast<std::uint16_t>(descriptor.free_blocks));
    StoreLittleEndian(raw + 14, static_cast<std::uint16_t>(descriptor.free_inodes));
    StoreLittleEndian(raw + 16, static_cast<std::uint16_t>(descriptor.used_directories));
    if (bytes.size() >= LargeGroupDescriptorSize)
    {
        StoreLittleEndian(raw + 44, static_cast<std::uint16_t>(descriptor.free_blocks >> 16));
        StoreLittleEndian(raw + 46, static_cast<std::uint16_t>(descriptor.free_inodes >> 16));
        StoreLittleEndian(raw + 48, static_cast<std::uint16_t>(descriptor.used_directories >> 16));
    }
}

} // namespace FeatherDisk::Ext
