// SPDX-License-Identifier: MIT OR Apache-2.0
#include "ext/extent.h"

#include "common/little_endian.h"
#include "ext/checksum.h"
#include "ext/ext2_filesystem.h"

#include <optional>

namespace FeatherDisk::Ext
{
namespace
{

std::uint16_t Load16(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return LoadLittleEndian<std::uint16_t>(bytes.data() + offset);
}

std::uint32_t Load32(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return LoadLittleEndian<std::uint32_t>(bytes.data() + offset);
}

std::span<const std::uint8_t> Entry(std::span<const std::uint8_t> node, std::size_t index)
{
    return node.subspan(ExtentHeaderSize + index * ExtentEntrySize, ExtentEntrySize);
}

} // namespace

Result<ExtentHeader> ParseExtentHeader(std::span<const std::uint8_t> node)
{
    if (node.size() < ExtentHeaderSize || Load16(node, 0) != ExtentMagic)
    {
        return std::unexpected(Error::Corrupt);
    }
    const ExtentHeader header = {
        .entries = Load16(node, 2),
        .maximum = Load16(node, 4),
        .depth = Load16(node, 6),
    };
    if (header.maximum == 0 || header.entries > header.maximum ||
        ExtentHeaderSize + std::size_t{header.maximum} * ExtentEntrySize > node.size())
    {
        return std::unexpected(Error::Corrupt);
    }
    return header;
}

Extent ExtentAt(std::span<const std::uint8_t> node, std::size_t index)
{
    const std::span<const std::uint8_t> entry = Entry(node, index);
    std::uint32_t length = Load16(entry, 4);
    const bool unwritten = length > MaximumInitializedLength;
    if (unwritten)
    {
        length -= MaximumInitializedLength;
    }
    return {
        .logical = Load32(entry, 0),
        .length = length,
        .physical = (std::uint64_t{Load16(entry, 6)} << 32) | Load32(entry, 8),
        .unwritten = unwritten,
    };
}

ExtentIndex ExtentIndexAt(std::span<const std::uint8_t> node, std::size_t index)
{
    const std::span<const std::uint8_t> entry = Entry(node, index);
    return {
        .logical = Load32(entry, 0),
        .child = (std::uint64_t{Load16(entry, 8)} << 32) | Load32(entry, 4),
    };
}

// The walk goes down one level per node and checks each child's depth against its parent's, so
// a damaged index cannot loop. Every node is checked whole on the way, which costs little next to
// reading its block: a caller maps a run of blocks per walk, not one block.
Result<BlockRun> Ext2Filesystem::MapExtent(const Inode& inode, std::uint64_t logical,
                                           std::span<std::uint8_t> scratch)
{
    // Linux takes the flag on a volume without the feature as damage.
    if (!extents || logical >= ExtentLogicalEnd)
    {
        return std::unexpected(Error::Corrupt);
    }
    std::span<const std::uint8_t> node(inode.blocks);
    Result<ExtentHeader> header = ParseExtentHeader(node);
    if (!header || header->depth > MaximumExtentDepth)
    {
        return std::unexpected(Error::Corrupt);
    }
    // The first logical block past the subtree being walked, from the next index entry above.
    std::uint64_t end = ExtentLogicalEnd;
    const std::size_t block_entries =
        (block_size - ExtentHeaderSize - ExtentTailSize) / ExtentEntrySize;
    while (header->depth > 0)
    {
        if (header->entries == 0)
        {
            return std::unexpected(Error::Corrupt);
        }
        std::size_t chosen = 0;
        for (std::size_t index = 0; index < header->entries; ++index)
        {
            const ExtentIndex entry = ExtentIndexAt(node, index);
            if (index > 0 && entry.logical <= ExtentIndexAt(node, index - 1).logical)
            {
                return std::unexpected(Error::Corrupt);
            }
            if (entry.logical <= logical)
            {
                chosen = index;
            }
        }
        const ExtentIndex first = ExtentIndexAt(node, 0);
        if (logical < first.logical)
        {
            const std::uint64_t until = first.logical < end ? first.logical : end;
            return BlockRun{.logical = logical, .physical = 0, .count = until - logical};
        }
        if (chosen + 1 < header->entries)
        {
            const std::uint64_t next = ExtentIndexAt(node, chosen + 1).logical;
            end = next < end ? next : end;
        }
        const std::uint64_t child = ExtentIndexAt(node, chosen).child;
        if (!IsDataBlock(child))
        {
            return std::unexpected(Error::Corrupt);
        }
        if (const Status read = cache.Read(child, 0, scratch, scratch); !read)
        {
            return Failure(read);
        }
        const std::uint16_t parent_depth = header->depth;
        node = scratch;
        header = ParseExtentHeader(node);
        if (!header || header->depth + 1 != parent_depth || header->maximum > block_entries)
        {
            return std::unexpected(Error::Corrupt);
        }
        if (metadata_checksums && !ExtentBlockChecksumMatches(node, inode.checksum_seed))
        {
            LogWithValue("ext2: extent block checksum mismatch in block ",
                         static_cast<std::uint32_t>(child));
            return std::unexpected(Error::Corrupt);
        }
    }

    // The whole leaf is checked, not just the extents before the one that maps logical.
    std::uint64_t previous_end = 0;
    std::uint64_t next = end;
    std::optional<BlockRun> found;
    for (std::size_t index = 0; index < header->entries; ++index)
    {
        const Extent extent = ExtentAt(node, index);
        const std::uint64_t extent_end = std::uint64_t{extent.logical} + extent.length;
        if (extent.length == 0 || extent.logical < previous_end || extent_end > ExtentLogicalEnd ||
            !IsDataBlock(extent.physical) ||
            extent.physical + extent.length > superblock.blocks_count)
        {
            return std::unexpected(Error::Corrupt);
        }
        previous_end = extent_end;
        if (found || logical >= extent_end)
        {
            continue;
        }
        if (logical < extent.logical)
        {
            next = extent.logical < next ? extent.logical : next;
            found = BlockRun{.logical = logical, .physical = 0, .count = next - logical};
            continue;
        }
        const std::uint64_t within = logical - extent.logical;
        std::uint64_t count = extent.length - within;
        if (end - logical < count)
        {
            count = end - logical;
        }
        found = BlockRun{
            .logical = logical,
            .physical = extent.unwritten ? 0 : extent.physical + within,
            .count = count,
        };
    }
    if (found)
    {
        return *found;
    }
    if (next <= logical)
    {
        return std::unexpected(Error::Corrupt);
    }
    return BlockRun{.logical = logical, .physical = 0, .count = next - logical};
}

} // namespace FeatherDisk::Ext
