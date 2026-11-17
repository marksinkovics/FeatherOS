// SPDX-License-Identifier: MIT OR Apache-2.0
// ext4's extent trees. Every node is a 12-byte header and entries of 12 bytes: index entries,
// each naming the block of a node one level down, above the leaves, and extents in them. The
// root fills i_block (four entries); every other node fills a block and, with metadata_csum, ends
// in a CRC32C right after its eh_max entries.
#ifndef FEATHERDISK_EXT_EXTENT_H
#define FEATHERDISK_EXT_EXTENT_H

#include "fs/error.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace FeatherDisk::Ext
{

constexpr std::uint16_t ExtentMagic = 0xF30A;
constexpr std::size_t ExtentHeaderSize = 12;
constexpr std::size_t ExtentEntrySize = 12;
constexpr std::size_t ExtentTailSize = 4;
// Linux never builds deeper trees, so a deeper root is damage.
constexpr std::uint16_t MaximumExtentDepth = 5;
// ee_len up to this is an extent of that many written blocks; above it, an unwritten extent
// (allocated, reading as zeros) of ee_len - 32768 blocks.
constexpr std::uint32_t MaximumInitializedLength = 32768;
// Logical block numbers are 32 bits.
constexpr std::uint64_t ExtentLogicalEnd = std::uint64_t{1} << 32;

struct ExtentHeader
{
    std::uint16_t entries;
    std::uint16_t maximum;
    // 0 in a leaf.
    std::uint16_t depth;
};

struct Extent
{
    std::uint32_t logical;
    std::uint32_t length;
    // 48 bits.
    std::uint64_t physical;
    bool unwritten;
};

struct ExtentIndex
{
    std::uint32_t logical;
    std::uint64_t child;
};

// Error::Corrupt unless the magic matches, eh_max is at least 1, its entries fit in node, and
// eh_entries is at most eh_max.
Result<ExtentHeader> ParseExtentHeader(std::span<const std::uint8_t> node);
// The entry at index of a node whose header said it holds that many.
Extent ExtentAt(std::span<const std::uint8_t> node, std::size_t index);
ExtentIndex ExtentIndexAt(std::span<const std::uint8_t> node, std::size_t index);

} // namespace FeatherDisk::Ext

#endif
