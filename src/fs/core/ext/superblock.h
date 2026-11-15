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
// Revision 0 reserves inodes 1 to 10; revision 1 says where the first ordinary inode is.
constexpr std::uint32_t GoodOldFirstInode = 11;

// s_state: clean is set while nothing is mounted for writing and cleared while it is, so a
// volume that was not unmounted shows it; errors stays until e2fsck clears it.
constexpr std::uint16_t StateClean = 0x0001;
constexpr std::uint16_t StateErrors = 0x0002;
// s_errors: what a driver does when it finds damage. Linux treats other values as remount-ro.
constexpr std::uint16_t ErrorsContinue = 1;
constexpr std::uint16_t ErrorsReadOnly = 2;
constexpr std::uint16_t ErrorsPanic = 3;

// s_feature_compat: a reader may ignore these, but a writer must keep what they promise. These
// ask nothing of this driver that it does not do: preallocation is a hint, extended attribute
// blocks are reference-counted when a file goes, the resize inode's blocks stay marked in the
// bitmaps, and a hashed directory loses its index flag when it changes, as Linux's ext2 does.
constexpr std::uint32_t CompatibleDirectoryPreallocation = 0x0001;
constexpr std::uint32_t CompatibleHasJournal = 0x0004;
constexpr std::uint32_t CompatibleExtendedAttributes = 0x0008;
constexpr std::uint32_t CompatibleResizeInode = 0x0010;
constexpr std::uint32_t CompatibleDirectoryIndex = 0x0020;
constexpr std::uint32_t CompatibleWritable = CompatibleDirectoryPreallocation |
                                             CompatibleExtendedAttributes | CompatibleResizeInode |
                                             CompatibleDirectoryIndex;

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
    // s_r_blocks_count: blocks kept for the superuser.
    std::uint32_t reserved_blocks_count;
    // Seconds since 1970 of the last mount and the last write of the superblock.
    std::uint32_t mount_time;
    std::uint32_t write_time;
    std::uint16_t mount_count;
    std::uint16_t state;
    std::uint16_t errors;
    std::uint32_t first_inode;
    // Blocks after each copy of the group descriptor table kept for online growth (resize_inode).
    std::uint16_t reserved_descriptor_blocks;
    // i_extra_isize every inode has and new inodes should get; 0 for 128-byte inodes.
    std::uint16_t minimum_extra_size;
    std::uint16_t wanted_extra_size;
};

// One block group's entry in the group descriptor table.
struct GroupDescriptor
{
    std::uint32_t block_bitmap;
    std::uint32_t inode_bitmap;
    std::uint32_t inode_table;
    std::uint16_t free_blocks;
    std::uint16_t free_inodes;
    std::uint16_t used_directories;
};

Superblock DecodeSuperblock(std::span<const std::uint8_t, SuperblockSize> bytes);
// The magic number and fields without which nothing else can be read: what Probe looks at.
bool LooksLikeExt(const Superblock& superblock);
// Fails with Error::Unsupported for a revision or block size this driver does not read, and with
// Error::Corrupt for a layout that contradicts itself or does not fit in volume_bytes.
Status CheckSuperblock(const Superblock& superblock, std::uint64_t volume_bytes);
// Stores what a mounted volume changes: the free counts, mount and write times, mount count,
// state and, from revision 1, the read-only compatible features. Every other byte is kept.
void EncodeSuperblockState(const Superblock& superblock,
                           std::span<std::uint8_t, SuperblockSize> bytes);
// Whether a group starts with a copy of the superblock and the group descriptor table: groups 0
// and 1 and the powers of 3, 5 and 7 with sparse_super, every group without it.
bool GroupHasSuperblock(const Superblock& superblock, std::uint32_t group);

GroupDescriptor DecodeGroupDescriptor(std::span<const std::uint8_t, GroupDescriptorSize> bytes);
// Stores the three counts; the locations never change.
void EncodeGroupCounts(const GroupDescriptor& descriptor,
                       std::span<std::uint8_t, GroupDescriptorSize> bytes);

} // namespace FeatherDisk::Ext

#endif
