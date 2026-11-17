// SPDX-License-Identifier: MIT OR Apache-2.0
// The checksums ext4 keeps in its metadata: CRC16 in the group descriptors (gdt_csum), or CRC32C
// (metadata_csum) in the superblock, descriptors, bitmaps, inodes and directory blocks, all seeded
// from the volume. Each function gives the value as stored, so callers compare it with the field.
#ifndef FEATHERDISK_EXT_CHECKSUM_H
#define FEATHERDISK_EXT_CHECKSUM_H

#include "ext/superblock.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace FeatherDisk::Ext
{

// The record at the end of every directory leaf block with metadata_csum: an unused entry of 12
// bytes whose file type byte is 0xDE, holding the block's CRC32C.
constexpr std::size_t DirectoryTailSize = 12;

// s_checksum_seed with metadata_csum_seed, otherwise the CRC32C of the UUID.
std::uint32_t VolumeChecksumSeed(const Superblock& superblock);
// What bg_checksum must hold for descriptor bytes (the superblock's descriptor size), or 0 when
// the volume has no group checksums.
std::uint16_t GroupDescriptorChecksum(const Superblock& superblock, std::uint32_t volume_seed,
                                      std::uint32_t group, std::span<const std::uint8_t> bytes);
// The block or inode bitmap's CRC32C over the bytes that hold the group's bits; the descriptor
// keeps the lower half only when it is 32 bytes.
std::uint32_t BitmapChecksum(std::uint32_t volume_seed, std::span<const std::uint8_t> bitmap);
// Inode and directory checksums continue from the inode's own seed.
std::uint32_t InodeChecksumSeed(std::uint32_t volume_seed, std::uint32_t inode,
                                std::uint32_t generation);
// raw is the whole on-disk inode. Inodes of 128 bytes, or whose i_extra_isize leaves out
// i_checksum_hi, keep only the lower 16 bits.
bool InodeChecksumMatches(std::span<const std::uint8_t> raw, std::uint32_t inode_seed);
// A leaf block must end in the checksum record and match it.
bool DirectoryBlockChecksumMatches(std::span<const std::uint8_t> block, std::uint32_t inode_seed);
// Whether a block of a hashed directory is an index block instead of a leaf: the first block, or
// one that starts with an unused record covering it all.
bool IsIndexBlock(std::span<const std::uint8_t> block, std::uint64_t logical);
// An index block (the root after "." and "..", or an interior node) must leave room after its
// entries' limit for the checksum, and match it.
bool IndexBlockChecksumMatches(std::span<const std::uint8_t> block, std::uint32_t inode_seed);

} // namespace FeatherDisk::Ext

#endif
