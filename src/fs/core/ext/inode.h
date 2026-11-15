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

    std::uint32_t Block(std::uint32_t index) const;
};

// raw holds the first min(inode size, DecodedInodeBytes) bytes of the inode, at least 128.
// read_only_compatible says whether the volume has huge files.
Inode DecodeInode(std::span<const std::uint8_t> raw, std::uint32_t read_only_compatible,
                  std::uint32_t block_size);
// nullopt for a type ext2 does not define.
std::optional<NodeType> TypeOf(std::uint16_t mode);

} // namespace FeatherDisk::Ext

#endif
