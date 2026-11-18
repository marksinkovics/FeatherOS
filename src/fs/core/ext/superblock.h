// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_EXT_SUPERBLOCK_H
#define FEATHERDISK_EXT_SUPERBLOCK_H

#include "fs/error.h"

#include <array>
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
// Group descriptors are 32 bytes, or s_desc_size (a power of two from 64) with 64bit.
constexpr std::uint32_t GroupDescriptorSize = 32;
constexpr std::uint32_t LargeGroupDescriptorSize = 64;
constexpr std::uint32_t MaximumGroupDescriptorSize = 1024;
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
// bitmaps, a hashed directory keeps its index while names leave it and loses it before one is
// added, and sparse_super2's backups are metadata like any other.
constexpr std::uint32_t CompatibleDirectoryPreallocation = 0x0001;
constexpr std::uint32_t CompatibleHasJournal = 0x0004;
constexpr std::uint32_t CompatibleExtendedAttributes = 0x0008;
constexpr std::uint32_t CompatibleResizeInode = 0x0010;
constexpr std::uint32_t CompatibleDirectoryIndex = 0x0020;
// Backups of the superblock only in group 0 and the two groups s_backup_bgs names.
constexpr std::uint32_t CompatibleSparseSuper2 = 0x0200;
// An orphan file (s_orphan_file_inum), which only matters while orphan_present is set; kept when
// the volume has a journal.
constexpr std::uint32_t CompatibleOrphanFile = 0x1000;
constexpr std::uint32_t CompatibleWritable =
    CompatibleDirectoryPreallocation | CompatibleHasJournal | CompatibleExtendedAttributes |
    CompatibleResizeInode | CompatibleDirectoryIndex | CompatibleSparseSuper2;

// s_feature_incompat: a driver must refuse a volume with a bit it does not know.
constexpr std::uint32_t IncompatibleCompression = 0x0001;
constexpr std::uint32_t IncompatibleFileType = 0x0002;
// The journal holds transactions not yet copied to their places (needs_recovery).
constexpr std::uint32_t IncompatibleRecover = 0x0004;
constexpr std::uint32_t IncompatibleJournalDevice = 0x0008;
constexpr std::uint32_t IncompatibleMetaGroups = 0x0010;
constexpr std::uint32_t IncompatibleExtents = 0x0040;
constexpr std::uint32_t Incompatible64Bit = 0x0080;
constexpr std::uint32_t IncompatibleMultipleMountProtection = 0x0100;
// Bitmaps and inode tables of several groups packed together, anywhere in the volume.
constexpr std::uint32_t IncompatibleFlexibleGroups = 0x0200;
constexpr std::uint32_t IncompatibleExtendedAttributeInodes = 0x0400;
constexpr std::uint32_t IncompatibleDirectoryData = 0x1000;
// The checksum seed is s_checksum_seed instead of the CRC of the UUID (metadata_csum_seed).
constexpr std::uint32_t IncompatibleChecksumSeed = 0x2000;
constexpr std::uint32_t IncompatibleLargeDirectory = 0x4000;
constexpr std::uint32_t IncompatibleInlineData = 0x8000;
constexpr std::uint32_t IncompatibleEncryption = 0x10000;
constexpr std::uint32_t IncompatibleCaseFolding = 0x20000;
constexpr std::uint32_t IncompatibleReadable =
    IncompatibleFileType | IncompatibleRecover | IncompatibleExtents | Incompatible64Bit |
    IncompatibleFlexibleGroups | IncompatibleChecksumSeed;
constexpr std::uint32_t IncompatibleWritable = IncompatibleFileType | IncompatibleExtents |
                                               Incompatible64Bit | IncompatibleFlexibleGroups |
                                               IncompatibleChecksumSeed;

// s_feature_ro_compat: a driver may read a volume with a bit it does not know, but not write it.
constexpr std::uint32_t ReadOnlyCompatibleSparseSuper = 0x0001;
constexpr std::uint32_t ReadOnlyCompatibleLargeFile = 0x0002;
constexpr std::uint32_t ReadOnlyCompatibleBtreeDirectory = 0x0004;
constexpr std::uint32_t ReadOnlyCompatibleHugeFile = 0x0008;
// CRC16 checksums in the group descriptors (uninit_bg), which may then leave groups unwritten.
constexpr std::uint32_t ReadOnlyCompatibleGroupChecksum = 0x0010;
constexpr std::uint32_t ReadOnlyCompatibleDirectoryLinks = 0x0020;
constexpr std::uint32_t ReadOnlyCompatibleExtraInodeSize = 0x0040;
// Bitmaps count clusters of blocks: blocks_per_group no longer describes them.
constexpr std::uint32_t ReadOnlyCompatibleBigAlloc = 0x0200;
// CRC32C checksums in the superblock, descriptors, bitmaps, inodes and directory blocks.
constexpr std::uint32_t ReadOnlyCompatibleMetadataChecksum = 0x0400;
constexpr std::uint32_t ReadOnlyCompatibleWritable =
    ReadOnlyCompatibleSparseSuper | ReadOnlyCompatibleLargeFile | ReadOnlyCompatibleHugeFile |
    ReadOnlyCompatibleGroupChecksum | ReadOnlyCompatibleDirectoryLinks |
    ReadOnlyCompatibleExtraInodeSize | ReadOnlyCompatibleMetadataChecksum;
// At least one slot of the orphan file holds an inode.
constexpr std::uint32_t ReadOnlyCompatibleOrphanPresent = 0x10000;
// Those the read path refuses: the others only matter to a writer.
constexpr std::uint32_t ReadOnlyCompatibleUnreadable = ReadOnlyCompatibleBigAlloc;

// s_checksum_type: the only algorithm defined.
constexpr std::uint8_t ChecksumTypeCrc32c = 1;
// Where s_feature_incompat and s_checksum sit in the superblock's bytes.
constexpr std::size_t SuperblockIncompatibleOffset = 96;
constexpr std::size_t SuperblockChecksumOffset = 1020;

// Counts and block numbers are 64-bit: with 64bit their upper halves are in later fields.
struct Superblock
{
    std::uint32_t inodes_count;
    std::uint64_t blocks_count;
    std::uint64_t free_blocks_count;
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
    std::uint64_t reserved_blocks_count;
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
    std::uint32_t descriptor_size;
    std::array<std::uint8_t, 16> uuid;
    std::uint8_t checksum_type;
    // s_checksum_seed, which counts only with metadata_csum_seed.
    std::uint32_t stored_checksum_seed;
    // s_backup_bgs, with sparse_super2.
    std::array<std::uint32_t, 2> backup_groups;
    // s_journal_inum: 0 when the journal is on another device.
    std::uint32_t journal_inode;
    // s_last_orphan: the head of the list of inodes a crash left to finish; 0 when empty.
    std::uint32_t last_orphan;
    // s_orphan_file_inum, with the orphan_file feature.
    std::uint32_t orphan_file_inode;
    // flex_bg packs the bitmaps and inode tables of 2^log_groups_per_flex groups together.
    std::uint32_t log_groups_per_flex;
    // Without metadata_csum always true; with it, whether s_checksum matches the bytes.
    bool checksum_matches;
};

// bg_flags, which only count with group checksums.
// The inode table and bitmap were never written: every inode of the group is free.
constexpr std::uint16_t GroupInodesUnused = 0x0001;
// The block bitmap was never written; the group's metadata is all that is used.
constexpr std::uint16_t GroupBlocksUnused = 0x0002;

// One block group's entry in the group descriptor table, the upper halves of a 64-byte one
// included.
struct GroupDescriptor
{
    std::uint64_t block_bitmap;
    std::uint64_t inode_bitmap;
    std::uint64_t inode_table;
    std::uint32_t free_blocks;
    std::uint32_t free_inodes;
    std::uint32_t used_directories;
    std::uint16_t flags;
    // bg_itable_unused: inodes at the end of the group's table never used.
    std::uint32_t unused_inodes;
    std::uint32_t block_bitmap_checksum;
    std::uint32_t inode_bitmap_checksum;
    std::uint16_t checksum;
};

Superblock DecodeSuperblock(std::span<const std::uint8_t, SuperblockSize> bytes);
// The magic number and fields without which nothing else can be read: what Probe looks at.
bool LooksLikeExt(const Superblock& superblock);
// Fails with Error::Unsupported for a revision or block size this driver does not read, and with
// Error::Corrupt for a layout that contradicts itself or does not fit in volume_bytes.
Status CheckSuperblock(const Superblock& superblock, std::uint64_t volume_bytes);
// Stores what a mounted volume changes: the free counts, mount and write times, mount count,
// state and, from revision 1, the incompatible and read-only compatible features and the head of
// the orphan list. Every other byte is kept.
void EncodeSuperblockState(const Superblock& superblock,
                           std::span<std::uint8_t, SuperblockSize> bytes);
// s_checksum over the other bytes, for a volume with metadata_csum.
void StoreSuperblockChecksum(std::span<std::uint8_t, SuperblockSize> bytes);
// Whether a group starts with a copy of the superblock and the group descriptor table: groups 0
// and 1 and the powers of 3, 5 and 7 with sparse_super, group 0 and the two s_backup_bgs with
// sparse_super2, every group without either.
bool GroupHasSuperblock(const Superblock& superblock, std::uint32_t group);
// Whether the descriptors carry checksums (gdt_csum or metadata_csum), which also makes their
// flags and unused-inode counts count.
bool HasGroupChecksums(const Superblock& superblock);
bool HasMetadataChecksums(const Superblock& superblock);

// bytes is one descriptor, 32 bytes or the superblock's descriptor size.
GroupDescriptor DecodeGroupDescriptor(std::span<const std::uint8_t> bytes);
// Stores the counts, flags, unused-inode count and bitmap checksums, each in as many bits as the
// descriptor's size holds; the locations never change, and bg_checksum is the caller's.
void EncodeGroupDescriptor(const GroupDescriptor& descriptor, std::span<std::uint8_t> bytes);

} // namespace FeatherDisk::Ext

#endif
