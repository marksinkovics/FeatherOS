// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_EXT_INODE_H
#define FEATHERDISK_EXT_INODE_H

#include "fs/filesystem.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace FeatherDisk::Ext
{

constexpr std::uint32_t RootInode = 2;
// i_block: 12 direct block numbers, then a single, a double and a triple indirect one.
constexpr std::uint32_t DirectBlocks = 12;
constexpr std::uint32_t SingleIndirectBlock = 12;
constexpr std::uint32_t DoubleIndirectBlock = 13;
constexpr std::uint32_t TripleIndirectBlock = 14;
constexpr std::uint32_t BlockPointers = 15;
constexpr std::size_t BlockArrayBytes = std::size_t{BlockPointers} * 4;
// The fields this driver reads end here, in the extra space of inodes larger than 128 bytes.
constexpr std::size_t DecodedInodeBytes = 160;

// i_flags
// A directory whose first block holds a hash index (dir_index) behind fake records.
constexpr std::uint32_t InodeEncrypted = 0x00000800;
constexpr std::uint32_t InodeIndexed = 0x00001000;
constexpr std::uint32_t InodeHugeFile = 0x00040000;
constexpr std::uint32_t InodeExtents = 0x00080000;
constexpr std::uint32_t InodeInlineData = 0x10000000;

// i_mode's type, in its top four bits.
constexpr std::uint16_t ModeTypeMask = 0xF000;
constexpr std::uint16_t ModeSocket = 0xC000;
constexpr std::uint16_t ModeSymbolicLink = 0xA000;
constexpr std::uint16_t ModeRegular = 0x8000;
constexpr std::uint16_t ModeBlockDevice = 0x6000;
constexpr std::uint16_t ModeDirectory = 0x4000;
constexpr std::uint16_t ModeCharacterDevice = 0x2000;
constexpr std::uint16_t ModeFifo = 0x1000;
constexpr std::uint16_t ModePermissions = 07777;

struct Inode
{
    std::uint16_t mode;
    std::uint32_t owner;
    std::uint32_t group;
    std::uint64_t size;
    std::uint16_t link_count;
    std::uint32_t flags;
    // Bytes of blocks the inode holds, indirect and extended-attribute blocks included.
    std::uint64_t allocated_bytes;
    Timestamp access_time;
    Timestamp modify_time;
    Timestamp change_time;
    // The block numbers, or a fast symbolic link's target.
    std::array<std::uint8_t, BlockArrayBytes> blocks;
    // i_dtime: seconds since 1970 when the inode was freed, 0 while in use.
    std::uint32_t deletion_time;
    // A block of extended attributes shared by reference count, or 0.
    std::uint32_t attribute_block;
    // l_i_file_acl_high, the block's bits 32 to 47 on a 64bit volume.
    std::uint16_t attribute_block_high;
    // i_extra_isize: the bytes past the first 128 that hold fields.
    std::uint16_t extra_size;
    Timestamp creation_time;
    // i_generation, which seeds the inode's checksums.
    std::uint32_t generation;
    // Where the CRC32C of the inode's directory blocks starts, with metadata_csum; else 0.
    std::uint32_t checksum_seed;

    std::uint32_t Block(std::uint32_t index) const;
    void SetBlock(std::uint32_t index, std::uint32_t block);
};

// raw holds the first min(inode size, DecodedInodeBytes) bytes of the inode, at least 128.
// read_only_compatible says whether the volume has huge files.
Inode DecodeInode(std::span<const std::uint8_t> raw, std::uint32_t read_only_compatible,
                  std::uint32_t block_size);
// Stores the fields this driver changes into raw, laid out as DecodeInode takes it, and keeps
// every other byte. allocated_bytes goes into i_blocks in 512-byte sectors; with huge_file, in
// 48 bits, and past them in blocks under the huge-file flag, as Linux stores it. Extended times
// are stored where i_extra_isize covers them.
void EncodeInode(const Inode& inode, std::span<std::uint8_t> raw,
                 std::uint32_t read_only_compatible, std::uint32_t block_size);
// nullopt for a type ext2 does not define.
std::optional<NodeType> TypeOf(std::uint16_t mode);

} // namespace FeatherDisk::Ext

#endif
