// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_EXT_SUPERBLOCK_H
#define FEATHERDISK_EXT_SUPERBLOCK_H

#include "fs/error.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace FeatherDisk::Ext
{

// The superblock always starts 1024 bytes into the volume, whatever the block size, leaving room
// for a boot sector.
constexpr std::uint64_t SuperblockOffset = 1024;
constexpr std::size_t SuperblockSize = 1024;
constexpr std::uint16_t Magic = 0xEF53;
constexpr std::uint32_t GoodOldRevision = 0;
constexpr std::uint32_t DynamicRevision = 1;
constexpr std::uint32_t GoodOldInodeSize = 128;
constexpr std::uint32_t GroupDescriptorSize = 32;
constexpr std::uint32_t MinimumBlockSize = 1024;
constexpr std::uint32_t MaximumBlockSize = 4096;

// s_feature_incompat: a driver must refuse a volume with a bit it does not know.
constexpr std::uint32_t IncompatibleFileType = 0x0002;
constexpr std::uint32_t IncompatibleRecover = 0x0004;
constexpr std::uint32_t IncompatibleExtents = 0x0040;
constexpr std::uint32_t IncompatibleSupported = IncompatibleFileType;

// s_feature_ro_compat: a driver may read a volume with a bit it does not know, but not write it.
constexpr std::uint32_t ReadOnlyCompatibleSparseSuper = 0x0001;
constexpr std::uint32_t ReadOnlyCompatibleLargeFile = 0x0002;
constexpr std::uint32_t ReadOnlyCompatibleHugeFile = 0x0008;
constexpr std::uint32_t ReadOnlyCompatibleUnderstood =
    ReadOnlyCompatibleSparseSuper | ReadOnlyCompatibleLargeFile;

struct Superblock
{
    std::uint32_t inodes_count;
    std::uint32_t blocks_count;
    std::uint32_t free_blocks_count;
    std::uint32_t free_inodes_count;
    std::uint32_t first_data_block;
    std::uint32_t log_block_size;
    // 1024 << log_block_size, or 0 when that does not fit.
    std::uint32_t block_size;
    std::uint32_t blocks_per_group;
    std::uint32_t inodes_per_group;
    std::uint16_t magic;
    std::uint32_t revision;
    std::uint32_t inode_size;
    std::uint32_t compatible;
    std::uint32_t incompatible;
    std::uint32_t read_only_compatible;
    // Derived from the counts; 0 when they make no sense.
    std::uint32_t group_count;
};

Superblock DecodeSuperblock(std::span<const std::uint8_t, SuperblockSize> bytes);
// The magic number and fields without which nothing else can be read: what Probe looks at.
bool LooksLikeExt(const Superblock& superblock);
// Fails with Error::Unsupported for a revision or block size this driver does not read, and with
// Error::Corrupt for a layout that contradicts itself or does not fit in volume_bytes.
Status CheckSuperblock(const Superblock& superblock, std::uint64_t volume_bytes);

} // namespace FeatherDisk::Ext

#endif
