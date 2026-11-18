// SPDX-License-Identifier: MIT OR Apache-2.0
// Changes to ext4's extent trees, under the write order of write.cpp. Every change ends in one
// write, the commit, that makes the tree say what it should: a node rewritten in place, or the
// inode when the root changes. Whatever the commit links is written before it: data blocks, and
// new tree blocks that take a node's entries when they no longer fit in it (a split), when its
// first logical block changes (the index above names it, and must change in the same write), or
// when the root, full, moves its entries into a block one level down. A node that loses its last
// entry leaves the node above it. Blocks the commit unlinks, data or tree, are freed after it.
// So a crash leaves the tree as before or after the change, with leaked blocks at most, never two
// nodes mapping the same block. Unwritten extents turn written only after the data is in their
// blocks, so they read as zeros or as what was written, never as the blocks' old contents. e2fsck
// ends the range of a tree's last index entry at i_size and clears extents past it, so a file or
// directory grows its size before blocks past it are mapped, and shrinks it after they are gone:
// a crash leaves a size past the last block, which is only a hole (or, for a directory, a size
// that preen mode corrects).
#include "common/bytes.h"
#include "common/little_endian.h"
#include "ext/checksum.h"
#include "ext/ext2_filesystem.h"
#include "ext/extent.h"

namespace FeatherDisk::Ext
{
namespace
{

// Each of the two lists holds a node's entries and two more, laid out as a node.
constexpr std::size_t ListBlocks = 2;

std::uint64_t EndOf(const Extent& extent)
{
    return std::uint64_t{extent.logical} + extent.length;
}

std::uint64_t Smaller(std::uint64_t first, std::uint64_t second)
{
    return first < second ? first : second;
}

void CopyEntries(std::span<std::uint8_t> to, std::size_t to_index,
                 std::span<const std::uint8_t> from, std::size_t from_index, std::size_t count)
{
    if (count == 0)
    {
        return;
    }
    CopyBytes(to.data() + ExtentHeaderSize + to_index * ExtentEntrySize,
              from.data() + ExtentHeaderSize + from_index * ExtentEntrySize,
              count * ExtentEntrySize);
}

bool SameEntries(std::span<const std::uint8_t> first, std::span<const std::uint8_t> second,
                 std::size_t count)
{
    for (std::size_t index = ExtentHeaderSize; index < ExtentHeaderSize + count * ExtentEntrySize;
         ++index)
    {
        if (first[index] != second[index])
        {
            return false;
        }
    }
    return true;
}

// Whether after can join before as one written extent.
bool Continues(const Extent& before, const Extent& after)
{
    return !before.unwritten && !after.unwritten && EndOf(before) == after.logical &&
           before.physical + before.length == after.physical &&
           before.length + after.length <= MaximumInitializedLength;
}

void ReleaseBytes(Inode& inode, std::uint64_t bytes)
{
    inode.allocated_bytes = bytes < inode.allocated_bytes ? inode.allocated_bytes - bytes : 0;
}

} // namespace

ExtentEdit Ext2Filesystem::BeginExtentEdit(NodeId node, Inode& inode, Buffers& tree,
                                           Buffers& buffers)
{
    return {
        .node = node,
        .inode = &inode,
        .tree = &tree,
        .scratch = buffers[ScratchBuffer],
        .bitmap = buffers[BitmapBuffer],
        .frees = {.bitmap = tree[ExtentFreeBuffer], .group = 0, .loaded = false, .dirty = false},
        .goal = static_cast<std::uint32_t>(GroupFirstBlock(GroupOfInode(node))),
        .fresh = {},
        .fresh_count = 0,
        .committing = false,
    };
}

// The same checks as MapExtent's walk, and a node below the root is never empty. With every node
// the leaf's extents are checked too: in order, inside the volume and below the next index entry.
Status Ext2Filesystem::FindExtentPath(const Inode& inode, std::uint64_t logical, ExtentPath& path,
                                      Buffers& tree)
{
    if (!extents || (inode.flags & InodeExtents) == 0 || logical >= ExtentLogicalEnd)
    {
        return std::unexpected(Error::Corrupt);
    }
    path.root = inode.blocks;
    path.end = ExtentLogicalEnd;
    std::span<std::uint8_t> node(path.root);
    Result<ExtentHeader> header = ParseExtentHeader(node);
    if (!header || header->depth > MaximumExtentDepth)
    {
        return std::unexpected(Error::Corrupt);
    }
    path.depth = header->depth;
    std::uint64_t block = 0;
    for (std::size_t level = 0;; ++level)
    {
        ExtentLevel& at = path.levels[level];
        at = {.block = block, .node = node, .entries = header->entries, .position = NoPosition};
        for (std::size_t index = 0; index < header->entries; ++index)
        {
            const std::uint32_t start = EntryLogical(node, index);
            if (index > 0 && start <= EntryLogical(node, index - 1))
            {
                return std::unexpected(Error::Corrupt);
            }
            if (start <= logical)
            {
                at.position = index;
            }
        }
        if (header->depth == 0)
        {
            break;
        }
        if (header->entries == 0)
        {
            return std::unexpected(Error::Corrupt);
        }
        if (at.position == NoPosition)
        {
            at.position = 0;
        }
        if (at.position + 1 < header->entries)
        {
            path.end = Smaller(path.end, EntryLogical(node, at.position + 1));
        }
        const std::uint64_t child = ExtentIndexAt(node, at.position).child;
        const std::span<std::uint8_t> below = tree[ExtentPathBuffers + level];
        header = LoadExtentNode(inode, child, header->depth, below);
        if (!header)
        {
            return Failure(header);
        }
        if (header->entries == 0)
        {
            return std::unexpected(Error::Corrupt);
        }
        node = below;
        block = child;
    }
    const ExtentLevel& leaf = path.levels[path.depth];
    std::uint64_t previous_end = 0;
    for (std::size_t index = 0; index < leaf.entries; ++index)
    {
        const Extent extent = ExtentAt(leaf.node, index);
        if (extent.length == 0 || extent.logical < previous_end || EndOf(extent) > path.end ||
            !IsDataBlock(extent.physical) ||
            extent.physical + extent.length > superblock.blocks_count)
        {
            return std::unexpected(Error::Corrupt);
        }
        previous_end = EndOf(extent);
    }
    return {};
}

// Frees blocks a change has given back first, so the allocator never reads a bitmap that a held
// one is about to overwrite.
Result<std::uint64_t> Ext2Filesystem::AllocateTreeBlock(ExtentEdit& edit)
{
    if (edit.fresh_count == edit.fresh.size())
    {
        return std::unexpected(Error::Corrupt);
    }
    if (const Status finished = FinishFrees(edit.frees); !finished)
    {
        return Failure(finished);
    }
    const Result<std::uint32_t> block = AllocateBlock(edit.goal, edit.bitmap);
    if (!block)
    {
        return Failure(block);
    }
    edit.fresh[edit.fresh_count++] = *block;
    edit.inode->allocated_bytes += block_size;
    return std::uint64_t{*block};
}

// After a failed change: its new tree blocks go back unless the commit was tried, which may have
// reached the disk.
void Ext2Filesystem::ForgetFresh(ExtentEdit& edit)
{
    if (!edit.committing)
    {
        for (std::size_t index = 0; index < edit.fresh_count; ++index)
        {
            if (FreeBlock(edit.frees, edit.fresh[index]))
            {
                ReleaseBytes(*edit.inode, block_size);
            }
        }
    }
    edit.fresh_count = 0;
    [[maybe_unused]] const Status finished = FinishFrees(edit.frees);
}

Status Ext2Filesystem::WriteTreeNode(ExtentEdit& edit, std::uint64_t block,
                                     std::span<const std::uint8_t> list, std::size_t first,
                                     std::size_t count, std::uint16_t depth)
{
    const std::span<std::uint8_t> out = (*edit.tree)[ExtentOutputBuffer];
    ZeroBytes(out);
    const std::uint16_t maximum = BlockExtentEntries(block_size);
    StoreExtentHeader(
        out, {.entries = static_cast<std::uint16_t>(count), .maximum = maximum, .depth = depth});
    CopyEntries(out, 0, list, first, count);
    if (metadata_checksums)
    {
        const std::size_t tail = ExtentHeaderSize + std::size_t{maximum} * ExtentEntrySize;
        StoreLittleEndian(out.data() + tail,
                          ExtentBlockChecksum(out, tail, edit.inode->checksum_seed));
    }
    return cache.Write(block, out);
}

Status Ext2Filesystem::ReplaceExtentNode(ExtentEdit& edit, const ExtentPath& path,
                                         std::size_t level, std::span<std::uint8_t> list,
                                         std::size_t count)
{
    const auto depth = static_cast<std::uint16_t>(path.depth - level);
    const ExtentLevel& at = path.levels[level];
    if (level == 0)
    {
        std::array<std::uint8_t, BlockArrayBytes> root{};
        if (count <= RootExtentEntries)
        {
            // An empty tree is a leaf again, as Linux leaves it.
            StoreExtentHeader(root, {.entries = static_cast<std::uint16_t>(count),
                                     .maximum = RootExtentEntries,
                                     .depth = count == 0 ? std::uint16_t{0} : depth});
            CopyEntries(root, 0, list, 0, count);
        }
        else
        {
            if (depth >= MaximumExtentDepth)
            {
                return std::unexpected(Error::FileTooLarge);
            }
            const Result<std::uint64_t> block = AllocateTreeBlock(edit);
            if (!block)
            {
                return Failure(block);
            }
            if (const Status written = WriteTreeNode(edit, *block, list, 0, count, depth); !written)
            {
                return written;
            }
            StoreExtentHeader(root, {.entries = 1,
                                     .maximum = RootExtentEntries,
                                     .depth = static_cast<std::uint16_t>(depth + 1)});
            StoreExtentIndex(root, 0, {.logical = EntryLogical(list, 0), .child = *block});
        }
        edit.inode->blocks = root;
        edit.committing = true;
        const Status stored = StoreInode(edit.node, *edit.inode, false, edit.scratch);
        if (stored)
        {
            edit.fresh_count = 0;
            edit.committing = false;
        }
        return stored;
    }

    const std::uint16_t capacity = BlockExtentEntries(block_size);
    const ExtentLevel& parent = path.levels[level - 1];
    const std::span<std::uint8_t> first_list = edit.tree->Blocks(ExtentListBuffers, ListBlocks);
    const std::span<std::uint8_t> above =
        list.data() == first_list.data()
            ? edit.tree->Blocks(ExtentListBuffers + ListBlocks, ListBlocks)
            : first_list;
    std::size_t above_count = parent.entries;
    // Whether at.block leaves the tree with the commit.
    bool replaced = true;
    if (count == 0)
    {
        CopyEntries(above, 0, parent.node, 0, parent.position);
        CopyEntries(above, parent.position, parent.node, parent.position + 1,
                    parent.entries - parent.position - 1);
        --above_count;
    }
    else if (count <= capacity && EntryLogical(list, 0) == EntryLogical(at.node, 0))
    {
        edit.committing = true;
        const Status written = WriteTreeNode(edit, at.block, list, 0, count, depth);
        if (written)
        {
            edit.fresh_count = 0;
            edit.committing = false;
        }
        return written;
    }
    else if (count <= capacity)
    {
        const Result<std::uint64_t> block = AllocateTreeBlock(edit);
        if (!block)
        {
            return Failure(block);
        }
        if (const Status written = WriteTreeNode(edit, *block, list, 0, count, depth); !written)
        {
            return written;
        }
        CopyEntries(above, 0, parent.node, 0, parent.entries);
        StoreExtentIndex(above, parent.position,
                         {.logical = EntryLogical(list, 0), .child = *block});
    }
    else
    {
        // Entries only added after the node's own keep it whole; the new ones start a sibling,
        // as appending a file does. Otherwise both halves go to new blocks.
        const bool appended = count > at.entries && SameEntries(list, at.node, at.entries);
        const std::size_t split = appended ? at.entries : count / 2;
        std::uint64_t left = at.block;
        if (!appended)
        {
            const Result<std::uint64_t> block = AllocateTreeBlock(edit);
            if (!block)
            {
                return Failure(block);
            }
            left = *block;
            if (const Status written = WriteTreeNode(edit, left, list, 0, split, depth); !written)
            {
                return written;
            }
        }
        const Result<std::uint64_t> right = AllocateTreeBlock(edit);
        if (!right)
        {
            return Failure(right);
        }
        if (const Status written = WriteTreeNode(edit, *right, list, split, count - split, depth);
            !written)
        {
            return written;
        }
        CopyEntries(above, 0, parent.node, 0, parent.position);
        StoreExtentIndex(above, parent.position, {.logical = EntryLogical(list, 0), .child = left});
        StoreExtentIndex(above, parent.position + 1,
                         {.logical = EntryLogical(list, split), .child = *right});
        CopyEntries(above, parent.position + 2, parent.node, parent.position + 1,
                    parent.entries - parent.position - 1);
        ++above_count;
        replaced = !appended;
    }
    if (const Status committed = ReplaceExtentNode(edit, path, level - 1, above, above_count);
        !committed)
    {
        return committed;
    }
    if (replaced)
    {
        if (const Status freed = FreeBlock(edit.frees, at.block); !freed)
        {
            return freed;
        }
        ReleaseBytes(*edit.inode, block_size);
    }
    return {};
}

Status Ext2Filesystem::InsertExtent(ExtentEdit& edit, const ExtentPath& path, const Extent& extent)
{
    edit.committing = false;
    const ExtentLevel& leaf = path.levels[path.depth];
    const std::span<std::uint8_t> list = edit.tree->Blocks(ExtentListBuffers, ListBlocks);
    std::size_t count = leaf.entries;
    if (leaf.position != NoPosition && Continues(ExtentAt(leaf.node, leaf.position), extent))
    {
        Extent merged = ExtentAt(leaf.node, leaf.position);
        merged.length += extent.length;
        CopyEntries(list, 0, leaf.node, 0, leaf.entries);
        StoreExtent(list, leaf.position, merged);
    }
    else
    {
        const std::size_t at = leaf.position == NoPosition ? 0 : leaf.position + 1;
        CopyEntries(list, 0, leaf.node, 0, at);
        StoreExtent(list, at, extent);
        CopyEntries(list, at + 1, leaf.node, at, leaf.entries - at);
        ++count;
    }
    Status status = ReplaceExtentNode(edit, path, path.depth, list, count);
    if (!status)
    {
        ForgetFresh(edit);
    }
    if (const Status finished = FinishFrees(edit.frees); status && !finished)
    {
        status = finished;
    }
    return status;
}

// The unwritten extent splits into up to three: unwritten before, written, unwritten after. The
// written part joins a written neighbour it continues, so a file written front to back through
// fallocated space keeps two extents.
Status Ext2Filesystem::MarkWritten(ExtentEdit& edit, const ExtentPath& path, std::uint64_t logical,
                                   std::uint64_t count)
{
    edit.committing = false;
    const ExtentLevel& leaf = path.levels[path.depth];
    const std::size_t at = leaf.position;
    const Extent old = ExtentAt(leaf.node, at);
    const std::span<std::uint8_t> list = edit.tree->Blocks(ExtentListBuffers, ListBlocks);
    Extent written = {
        .logical = static_cast<std::uint32_t>(logical),
        .length = static_cast<std::uint32_t>(count),
        .physical = old.physical + (logical - old.logical),
        .unwritten = false,
    };
    const Extent before = {
        .logical = old.logical,
        .length = static_cast<std::uint32_t>(logical - old.logical),
        .physical = old.physical,
        .unwritten = true,
    };
    const Extent after = {
        .logical = static_cast<std::uint32_t>(logical + count),
        .length = static_cast<std::uint32_t>(EndOf(old) - (logical + count)),
        .physical = written.physical + count,
        .unwritten = true,
    };
    CopyEntries(list, 0, leaf.node, 0, at);
    std::size_t length = at;
    bool placed = false;
    if (before.length == 0 && at > 0 && Continues(ExtentAt(leaf.node, at - 1), written))
    {
        Extent merged = ExtentAt(leaf.node, at - 1);
        merged.length += written.length;
        StoreExtent(list, at - 1, merged);
        placed = true;
    }
    if (before.length > 0)
    {
        StoreExtent(list, length++, before);
    }
    std::size_t next = at + 1;
    if (!placed)
    {
        if (after.length == 0 && next < leaf.entries &&
            Continues(written, ExtentAt(leaf.node, next)))
        {
            written.length += ExtentAt(leaf.node, next).length;
            ++next;
        }
        StoreExtent(list, length++, written);
    }
    if (after.length > 0)
    {
        StoreExtent(list, length++, after);
    }
    CopyEntries(list, length, leaf.node, next, leaf.entries - next);
    length += leaf.entries - next;
    Status status = ReplaceExtentNode(edit, path, path.depth, list, length);
    if (!status)
    {
        ForgetFresh(edit);
    }
    if (const Status finished = FinishFrees(edit.frees); status && !finished)
    {
        status = finished;
    }
    return status;
}

// From the right: each round cuts the last leaf down to keep and frees what it lost, until a
// leaf keeps an entry.
Status Ext2Filesystem::ShrinkExtents(NodeId node, Inode& inode, std::uint64_t keep,
                                     Buffers& buffers)
{
    Buffers tree(block_size, ExtentBuffers);
    if (tree.Empty())
    {
        return std::unexpected(Error::OutOfMemory);
    }
    ExtentEdit edit = BeginExtentEdit(node, inode, tree, buffers);
    ExtentPath path{};
    for (;;)
    {
        edit.committing = false;
        if (const Status found = FindExtentPath(inode, ExtentLogicalEnd - 1, path, tree); !found)
        {
            return found;
        }
        const ExtentLevel& leaf = path.levels[path.depth];
        std::size_t cut = leaf.entries;
        while (cut > 0 && EndOf(ExtentAt(leaf.node, cut - 1)) > keep)
        {
            --cut;
        }
        if (cut == leaf.entries)
        {
            return {};
        }
        const std::span<std::uint8_t> list = tree.Blocks(ExtentListBuffers, ListBlocks);
        CopyEntries(list, 0, leaf.node, 0, cut);
        std::size_t count = cut;
        if (const Extent straddling = ExtentAt(leaf.node, cut); straddling.logical < keep)
        {
            Extent kept = straddling;
            kept.length = static_cast<std::uint32_t>(keep - straddling.logical);
            StoreExtent(list, count++, kept);
        }
        if (const Status replaced = ReplaceExtentNode(edit, path, path.depth, list, count);
            !replaced)
        {
            ForgetFresh(edit);
            return replaced;
        }
        Status status;
        for (std::size_t index = cut; index < leaf.entries && status; ++index)
        {
            const Extent extent = ExtentAt(leaf.node, index);
            const std::uint64_t from = extent.logical < keep ? keep - extent.logical : 0;
            status = FreeBlocks(edit.frees, extent.physical + from, extent.length - from);
            ReleaseBytes(inode, (extent.length - from) * block_size);
        }
        const Status finished = FinishFrees(edit.frees);
        if (!status)
        {
            return status;
        }
        if (!finished)
        {
            return finished;
        }
        if (count > 0)
        {
            return {};
        }
    }
}

// tree[depth] holds the nodes of each depth below the root on the way down.
Status Ext2Filesystem::FreeExtentSubtree(const Inode& inode, std::span<const std::uint8_t> node,
                                         std::uint16_t depth, Buffers& tree, FreeCursor& frees)
{
    const Result<ExtentHeader> header = ParseExtentHeader(node);
    if (!header)
    {
        return Failure(header);
    }
    for (std::size_t index = 0; index < header->entries; ++index)
    {
        if (depth == 0)
        {
            const Extent extent = ExtentAt(node, index);
            if (extent.length == 0 || !IsDataBlock(extent.physical) ||
                extent.physical + extent.length > superblock.blocks_count)
            {
                return std::unexpected(Error::Corrupt);
            }
            if (const Status freed = FreeBlocks(frees, extent.physical, extent.length); !freed)
            {
                return freed;
            }
            continue;
        }
        const std::uint64_t child = ExtentIndexAt(node, index).child;
        const std::span<std::uint8_t> below = tree[depth - 1U];
        if (const Result<ExtentHeader> loaded = LoadExtentNode(inode, child, depth, below); !loaded)
        {
            return Failure(loaded);
        }
        if (const Status freed =
                FreeExtentSubtree(inode, below, static_cast<std::uint16_t>(depth - 1), tree, frees);
            !freed)
        {
            return freed;
        }
        if (const Status freed = FreeBlock(frees, child); !freed)
        {
            return freed;
        }
    }
    return {};
}

Status Ext2Filesystem::ReleaseExtents(const Inode& inode, FreeCursor& frees)
{
    const Result<ExtentHeader> header = ParseExtentHeader(inode.blocks);
    if (!header || header->depth > MaximumExtentDepth)
    {
        return std::unexpected(Error::Corrupt);
    }
    Buffers tree(block_size, MaximumExtentDepth);
    if (tree.Empty())
    {
        return std::unexpected(Error::OutOfMemory);
    }
    return FreeExtentSubtree(inode, inode.blocks, header->depth, tree, frees);
}

Status Ext2Filesystem::WriteDataBlocks(std::uint64_t physical, std::uint64_t logical,
                                       std::uint64_t count, std::uint64_t offset,
                                       std::span<const std::uint8_t> data, bool fresh,
                                       std::span<std::uint8_t> block)
{
    const std::uint64_t data_end = offset + data.size();
    std::uint64_t index = 0;
    while (index < count)
    {
        const std::uint64_t start = (logical + index) * block_size;
        if (start >= offset && start + block_size <= data_end)
        {
            // Whole blocks go to the device straight from the caller's bytes, in one write.
            std::uint64_t whole = 1;
            while (index + whole < count && start + (whole + 1) * block_size <= data_end)
            {
                ++whole;
            }
            if (const Status written =
                    cache.Write(physical + index, data.subspan(start - offset, whole * block_size));
                !written)
            {
                return written;
            }
            index += whole;
            continue;
        }
        if (fresh)
        {
            ZeroBytes(block);
        }
        else if (const Status read = cache.ReadUncached(physical + index, block); !read)
        {
            return read;
        }
        const std::uint64_t from = start > offset ? start : offset;
        const std::uint64_t to = Smaller(start + block_size, data_end);
        CopyBytes(block.data() + (from - start), data.data() + (from - offset), to - from);
        if (const Status written = cache.Write(physical + index, block); !written)
        {
            return written;
        }
        ++index;
    }
    return {};
}

// Run by run: blocks an extent maps are written in place, unwritten ones written and then marked
// written, and holes get new blocks near the ones before, written and then mapped.
Status Ext2Filesystem::WriteExtentFile(NodeId file, Inode& inode, std::uint64_t offset,
                                       std::span<const std::uint8_t> data, Buffers& buffers,
                                       std::size_t& written)
{
    Buffers tree(block_size, ExtentBuffers);
    if (tree.Empty())
    {
        return std::unexpected(Error::OutOfMemory);
    }
    ExtentEdit edit = BeginExtentEdit(file, inode, tree, buffers);
    const std::span<std::uint8_t> block = buffers[DataBuffer];
    std::uint64_t goal = edit.goal;
    if (offset / block_size > 0)
    {
        if (const Result<std::uint64_t> before =
                PhysicalBlock(inode, offset / block_size - 1, buffers[ScratchBuffer]);
            before && *before != 0)
        {
            goal = *before + 1;
        }
    }
    const std::uint64_t last = (offset + data.size() - 1) / block_size;
    ExtentPath path{};
    while (written < data.size())
    {
        const std::uint64_t logical = (offset + written) / block_size;
        const std::uint64_t wanted = last - logical + 1;
        if (const Status found = FindExtentPath(inode, logical, path, tree); !found)
        {
            return found;
        }
        const ExtentLevel& leaf = path.levels[path.depth];
        std::optional<Extent> mapped;
        if (leaf.position != NoPosition)
        {
            if (const Extent extent = ExtentAt(leaf.node, leaf.position); logical < EndOf(extent))
            {
                mapped = extent;
            }
        }
        std::uint64_t physical = 0;
        std::uint64_t run = 0;
        if (mapped)
        {
            run = Smaller(EndOf(*mapped) - logical, wanted);
            physical = mapped->physical + (logical - mapped->logical);
            if (const Status stored =
                    WriteDataBlocks(physical, logical, run, offset, data, mapped->unwritten, block);
                !stored)
            {
                return stored;
            }
            if (mapped->unwritten)
            {
                if (const Status marked = MarkWritten(edit, path, logical, run); !marked)
                {
                    return marked;
                }
            }
        }
        else
        {
            std::uint64_t hole_end = path.end;
            const std::size_t next = leaf.position == NoPosition ? 0 : leaf.position + 1;
            if (next < leaf.entries)
            {
                hole_end = Smaller(hole_end, EntryLogical(leaf.node, next));
            }
            run = Smaller(Smaller(hole_end - logical, wanted), MaximumInitializedLength);
            const Result<AllocatedRun> allocated = AllocateBlocks(
                static_cast<std::uint32_t>(goal), static_cast<std::uint32_t>(run), edit.bitmap);
            if (!allocated)
            {
                return Failure(allocated);
            }
            physical = allocated->first;
            run = allocated->count;
            inode.allocated_bytes += run * block_size;
            Status status = WriteDataBlocks(physical, logical, run, offset, data, true, block);
            if (status)
            {
                status = InsertExtent(edit, path,
                                      {.logical = static_cast<std::uint32_t>(logical),
                                       .length = static_cast<std::uint32_t>(run),
                                       .physical = physical,
                                       .unwritten = false});
            }
            if (!status)
            {
                // Never linked, unless the commit was tried.
                if (!edit.committing && FreeBlocks(edit.frees, physical, run))
                {
                    ReleaseBytes(inode, run * block_size);
                }
                [[maybe_unused]] const Status finished = FinishFrees(edit.frees);
                return status;
            }
        }
        goal = physical + run;
        written = static_cast<std::size_t>(
            Smaller((logical + run) * block_size, offset + data.size()) - offset);
    }
    return {};
}

Status Ext2Filesystem::AppendExtentBlock(NodeId directory, Inode& inode,
                                         std::span<std::uint8_t> contents, Buffers& buffers)
{
    Buffers tree(block_size, ExtentBuffers);
    if (tree.Empty())
    {
        return std::unexpected(Error::OutOfMemory);
    }
    ExtentEdit edit = BeginExtentEdit(directory, inode, tree, buffers);
    const std::uint64_t logical = inode.size / block_size;
    ExtentPath path{};
    if (const Status found = FindExtentPath(inode, logical, path, tree); !found)
    {
        return found;
    }
    const ExtentLevel& leaf = path.levels[path.depth];
    std::uint64_t goal = edit.goal;
    if (leaf.position != NoPosition)
    {
        const Extent before = ExtentAt(leaf.node, leaf.position);
        if (logical < EndOf(before))
        {
            // A block past the directory's end.
            return std::unexpected(Error::Corrupt);
        }
        goal = before.physical + before.length;
    }
    const Result<AllocatedRun> allocated =
        AllocateBlocks(static_cast<std::uint32_t>(goal), 1, edit.bitmap);
    if (!allocated)
    {
        return Failure(allocated);
    }
    inode.allocated_bytes += block_size;
    // The size first, as for files: a crash leaves it past the last block, which e2fsck cuts.
    Status status = WriteDirectoryBlock(inode, logical, allocated->first, contents);
    if (status)
    {
        inode.size += block_size;
        status = StoreInode(directory, inode, false, edit.scratch);
    }
    if (status)
    {
        status = InsertExtent(edit, path,
                              {.logical = static_cast<std::uint32_t>(logical),
                               .length = 1,
                               .physical = allocated->first,
                               .unwritten = false});
    }
    if (!status && !edit.committing)
    {
        inode.size = logical * block_size;
        if (FreeBlock(edit.frees, allocated->first))
        {
            ReleaseBytes(inode, block_size);
        }
        [[maybe_unused]] const Status finished = FinishFrees(edit.frees);
        [[maybe_unused]] const Status stored = StoreInode(directory, inode, false, edit.scratch);
    }
    return status;
}

} // namespace FeatherDisk::Ext
