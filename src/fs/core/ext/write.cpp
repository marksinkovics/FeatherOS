// SPDX-License-Identifier: MIT OR Apache-2.0
// Changes to an ext2 volume's files; namespace.cpp changes its directory tree under the same
// rules. Without a journal, the order of the writes is what keeps a volume whole across a crash:
//   - a block or inode is marked in its bitmap before anything points to it;
//   - a new block's contents (file data, a zeroed indirect block, a directory block) are written
//     before the pointer to it, indirect blocks before the inode that holds them, and an inode
//     before the directory entry that names it;
//   - a link count goes up before the entry (or a new directory's "..") that adds the link is
//     written, and down only after it is gone;
//   - removing runs the other way: the entry goes first, then the inode's link, and the blocks
//     and the inode go back to their bitmaps last.
// A crash can then leave leaked blocks and inodes, free counts, sizes and i_blocks that lag, an
// unattached inode or a link count one too high, all of which e2fsck repairs in preen mode, but
// never a pointer to a block or an inode that is free or not yet written. The volume is marked
// not clean before the first change, so after a crash it mounts read-only until e2fsck has run.
// The order is that of BlockDevice writes, which reach the device at once (write-through); only
// Sync's Flush is a barrier, so a device that reorders completed writes in a volatile cache can
// still break it on power loss. The group descriptors and the superblock's counts are kept in
// memory and written by Sync, so after a crash e2fsck also finds bitmap checksums (metadata_csum)
// that lag behind their bitmaps; descriptor changes that say more of a group is in use are
// written at once (allocation.cpp). Every block written carries its checksums: the superblock,
// inodes, directory leaves and index blocks, and extended attribute blocks.
#include "common/bytes.h"
#include "common/little_endian.h"
#include "ext/ext2_filesystem.h"
#include "fs/host.h"

namespace FeatherDisk::Ext
{
namespace
{

constexpr std::uint64_t MaximumSmallFileBytes = 0x7FFFFFFF;
// i_size is 32 bits for a directory.
constexpr std::uint64_t MaximumDirectoryBytes = 0xFFFFFFFF;
constexpr std::uint32_t MaximumNanoseconds = 999'999'999;
// The header of an extended attribute block: h_magic, then h_refcount, the inodes sharing it.
constexpr std::uint32_t AttributeMagic = 0xEA020000;
constexpr std::size_t AttributeReferences = 4;
// h_blocks: always 1, as Linux requires.
constexpr std::size_t AttributeBlocks = 8;
constexpr std::size_t AttributeChecksum = 16;
// A hash index's root: "." in 12 bytes, ".." over the rest of the block.
constexpr std::size_t DotDotOffset = 12;

// Three each: the indirect blocks on the way to a shrinking file's new end, as read and as cut.
constexpr std::size_t ShrinkOriginals = 6;
constexpr std::size_t ShrinkChanged = 9;
constexpr std::size_t ShrinkBuffers = 12;

std::uint32_t LoadPointer(std::span<const std::uint8_t> block, std::uint32_t index)
{
    return LoadLittleEndian<std::uint32_t>(block.data() + std::size_t{index} * 4);
}

void StorePointer(std::span<std::uint8_t> block, std::uint32_t index, std::uint32_t value)
{
    StoreLittleEndian(block.data() + std::size_t{index} * 4, value);
}

std::uint32_t RootDepth(std::uint32_t root)
{
    return root < DirectBlocks ? 0 : root - DirectBlocks + 1;
}

// The logical blocks one pointer at depth covers.
std::uint64_t Span(std::uint32_t depth, std::uint64_t per_block)
{
    std::uint64_t span = 1;
    for (std::uint32_t level = 0; level < depth; ++level)
    {
        span *= per_block;
    }
    return span;
}

// The first logical block under i_block[root].
std::uint64_t RootFirst(std::uint32_t root, std::uint64_t per_block)
{
    std::uint64_t first = root < DirectBlocks ? root : DirectBlocks;
    for (std::uint32_t depth = 1; depth < RootDepth(root); ++depth)
    {
        first += Span(depth, per_block);
    }
    return first;
}

void ReleaseBytes(Inode& inode, std::uint64_t bytes)
{
    inode.allocated_bytes = bytes < inode.allocated_bytes ? inode.allocated_bytes - bytes : 0;
}

} // namespace

Timestamp Ext2Filesystem::Now() const
{
    constexpr std::uint64_t NanosecondsPerSecond = 1'000'000'000;
    if (const std::optional<std::uint64_t> now = Host::WallClockNanoseconds())
    {
        return {.seconds = static_cast<std::int64_t>(*now / NanosecondsPerSecond),
                .nanoseconds = static_cast<std::uint32_t>(*now % NanosecondsPerSecond)};
    }
    // Without a clock the volume's last write keeps times plausible, and i_dtime above the inode
    // numbers e2fsck takes for links in the orphan list.
    return {.seconds = superblock.write_time, .nanoseconds = 0};
}

bool Ext2Filesystem::IsReserved(NodeId node) const
{
    return node != RootInode && node < superblock.first_inode;
}

Status Ext2Filesystem::WriteSuperblock(std::span<std::uint8_t> scratch)
{
    // Only the primary copy is written, as Linux's ext2 does: the backups in later groups keep
    // the geometry and features, which no change here touches but large_file (which e2fsck does
    // not compare), and e2fsck recounts what is free whenever it falls back on one.
    const std::uint64_t block = SuperblockOffset / block_size;
    const std::size_t offset = SuperblockOffset % block_size;
    if (const Status read = cache.Read(block, 0, scratch, scratch); !read)
    {
        return read;
    }
    const std::span<std::uint8_t, SuperblockSize> bytes =
        scratch.subspan(offset).first<SuperblockSize>();
    EncodeSuperblockState(superblock, bytes);
    if (metadata_checksums)
    {
        StoreSuperblockChecksum(bytes);
    }
    return cache.Write(block, scratch);
}

Status Ext2Filesystem::MarkMounted()
{
    Buffers buffers(block_size, 1);
    if (buffers.Empty())
    {
        return std::unexpected(Error::OutOfMemory);
    }
    // The free counts are what the descriptors say, as e2fsck and Linux count them.
    std::uint64_t free_blocks = 0;
    std::uint64_t free_inodes = 0;
    for (std::uint32_t group = 0; group < superblock.group_count; ++group)
    {
        free_blocks += Group(group).free_blocks;
        free_inodes += Group(group).free_inodes;
    }
    const Superblock before = superblock;
    superblock.free_blocks_count = free_blocks;
    superblock.free_inodes_count = static_cast<std::uint32_t>(free_inodes);
    ++superblock.mount_count;
    superblock.mount_time = static_cast<std::uint32_t>(Now().seconds);
    superblock.state = static_cast<std::uint16_t>(superblock.state & ~StateClean);
    const Status written = WriteSuperblock(buffers[0]);
    if (!written && written.error() == Error::ReadOnly)
    {
        LogWarning("ext2: mounted read-only: ", "the device is read-only");
        superblock = before;
        read_only = true;
        return {};
    }
    if (!written)
    {
        return written;
    }
    writable_mount = true;
    superblock_clean = false;
    return volume.Flush();
}

Status Ext2Filesystem::BeginChange(std::span<std::uint8_t> scratch)
{
    if (read_only)
    {
        return Refused();
    }
    if (!superblock_clean)
    {
        return {};
    }
    superblock.state = static_cast<std::uint16_t>(superblock.state & ~StateClean);
    if (const Status written = WriteSuperblock(scratch); !written)
    {
        return written;
    }
    superblock_clean = false;
    return volume.Flush();
}

// s_errors says what to do about damage: carry on, or stop writing (remount-ro). The core cannot
// panic its host, so panic and unknown values stop writes too, as Linux's ext2 treats the latter.
void Ext2Filesystem::NoteError(Error error)
{
    if (error != Error::Corrupt || !writable_mount || (superblock.state & StateErrors) != 0)
    {
        return;
    }
    superblock.state = static_cast<std::uint16_t>(superblock.state | StateErrors);
    if (superblock.errors == ErrorsContinue && !read_only)
    {
        LogWarning("ext2: damage found; ", "writes go on, as s_errors says");
    }
    else
    {
        LogWarning("ext2: damage found; ", "no more writes until e2fsck has run");
        read_only = true;
    }
    Buffers buffers(block_size, 1);
    if (!buffers.Empty())
    {
        // Best effort: Sync writes the state again.
        [[maybe_unused]] const Status written = WriteSuperblock(buffers[0]);
    }
}

Status Ext2Filesystem::StoreInode(NodeId node, const Inode& inode, bool fresh,
                                  std::span<std::uint8_t> scratch)
{
    const Result<InodePlace> place = LocateInode(node);
    if (!place)
    {
        return Failure(place);
    }
    if (const Status read = cache.Read(place->block, 0, scratch, scratch); !read)
    {
        return read;
    }
    const std::span<std::uint8_t> slot = scratch.subspan(place->offset, superblock.inode_size);
    if (fresh)
    {
        ZeroBytes(slot);
    }
    EncodeInode(inode,
                slot.first(superblock.inode_size < DecodedInodeBytes ? superblock.inode_size
                                                                     : DecodedInodeBytes),
                superblock.read_only_compatible, block_size);
    if (metadata_checksums)
    {
        StoreInodeChecksum(slot,
                           InodeChecksumSeed(volume_seed, static_cast<std::uint32_t>(node),
                                             LoadLittleEndian<std::uint32_t>(slot.data() + 100)));
    }
    return cache.Write(place->block, scratch);
}

// The feature goes on before the inode that needs it, as on Linux.
Status Ext2Filesystem::SetLargeFile(std::uint64_t size, std::span<std::uint8_t> scratch)
{
    if (size <= MaximumSmallFileBytes ||
        (superblock.read_only_compatible & ReadOnlyCompatibleLargeFile) != 0)
    {
        return {};
    }
    superblock.read_only_compatible |= ReadOnlyCompatibleLargeFile;
    return WriteSuperblock(scratch);
}

Status Ext2Filesystem::WriteDirectoryBlock(const Inode& directory, std::uint64_t logical,
                                           std::uint64_t physical, std::span<std::uint8_t> block)
{
    if (metadata_checksums)
    {
        if (IsIndexOf(directory, logical, block))
        {
            if (!StoreIndexBlockChecksum(block, directory.checksum_seed))
            {
                return std::unexpected(Error::Corrupt);
            }
        }
        else
        {
            StoreDirectoryTail(block, directory.checksum_seed);
        }
    }
    return cache.Write(physical, block);
}

// A name added to a hashed directory would have to go into the leaf its hash picks, so the
// directory drops the index before any of its blocks change, as Linux's ext2 does; removing a
// name leaves the index right. Without checksums its blocks then read as a plain directory. With
// them, the blocks that held the index need a leaf's checksum record: the root's ".." and an
// interior node's empty record each give up their last 12 bytes for it. The order is the one
// e2fsck's preen mode repairs at every cut: the root first (e2fsck still reads the index through
// it, whose leaf checksum now matches), then the flag, then the interior nodes, which until then
// are a plain directory's blocks without a checksum record that e2fsck salvages.
Status Ext2Filesystem::ClearIndex(NodeId directory, Inode& inode, Buffers& buffers)
{
    if ((inode.flags & InodeIndexed) == 0)
    {
        return {};
    }
    const Inode indexed = inode;
    inode.flags &= ~InodeIndexed;
    if (!metadata_checksums || !hashed_directories)
    {
        return StoreInode(directory, inode, false, buffers[ScratchBuffer]);
    }
    const std::span<std::uint8_t> block = buffers[DataBuffer];
    const auto convert = [&](std::uint64_t logical) -> Status
    {
        const Result<std::uint32_t> physical = MapBlock(indexed, logical, buffers[ScratchBuffer]);
        if (!physical)
        {
            return Failure(physical);
        }
        if (*physical == 0)
        {
            return {};
        }
        if (const Status read = cache.Read(*physical, 0, block, block); !read)
        {
            return read;
        }
        if (!IsIndexOf(indexed, logical, block))
        {
            return {};
        }
        if (!IndexBlockChecksumMatches(block, indexed.checksum_seed))
        {
            return std::unexpected(Error::Corrupt);
        }
        // IndexBlockChecksumMatches has seen the records it relies on: the root's "." of 12
        // bytes and ".." over the rest, or one record over the whole node.
        const std::size_t record = logical == 0 ? DotDotOffset : 0;
        const auto length = LoadLittleEndian<std::uint16_t>(block.data() + record + 4);
        StoreLittleEndian(block.data() + record + 4,
                          static_cast<std::uint16_t>(length - DirectoryTailSize));
        return WriteDirectoryBlock(inode, logical, *physical, block);
    };
    if (const Status converted = convert(0); !converted)
    {
        return converted;
    }
    if (const Status stored = StoreInode(directory, inode, false, buffers[ScratchBuffer]); !stored)
    {
        return stored;
    }
    for (std::uint64_t logical = 1; logical < indexed.size / block_size; ++logical)
    {
        if (const Status converted = convert(logical); !converted)
        {
            return converted;
        }
    }
    return {};
}

// The bytes from position to the end of its block, so that what lay past the end of the file
// reads as zeros once the file reaches over it.
Status Ext2Filesystem::ZeroFrom(const Inode& inode, std::uint64_t position,
                                std::span<std::uint8_t> scratch)
{
    const auto within = static_cast<std::size_t>(position % block_size);
    if (within == 0)
    {
        return {};
    }
    const Result<std::uint32_t> physical = MapBlock(inode, position / block_size, scratch);
    if (!physical)
    {
        return Failure(physical);
    }
    if (*physical == 0)
    {
        return {};
    }
    if (const Status read = cache.ReadUncached(*physical, scratch); !read)
    {
        return read;
    }
    ZeroBytes(scratch.subspan(within));
    return cache.Write(*physical, scratch);
}

Result<std::uint32_t> Ext2Filesystem::AllocateFor(MapCursor& cursor, std::span<std::uint8_t> bitmap)
{
    const Result<std::uint32_t> block = AllocateBlock(cursor.goal, bitmap);
    if (block)
    {
        cursor.goal = *block + 1;
        ++cursor.allocated;
    }
    return block;
}

// Deeper levels first: an indirect block reaches the disk before the one that points to it.
Status Ext2Filesystem::ReleaseLevels(MapCursor& cursor, std::uint32_t from)
{
    for (std::uint32_t level = static_cast<std::uint32_t>(cursor.levels.size()); level-- > from;)
    {
        MapCursor::Level& held = cursor.levels[level];
        if (held.block != 0 && held.dirty)
        {
            if (const Status written = cache.Write(held.block, held.data); !written)
            {
                return written;
            }
        }
        held.block = 0;
        held.dirty = false;
    }
    return {};
}

Result<std::uint32_t> Ext2Filesystem::ResolveForWrite(Inode& inode, MapCursor& cursor,
                                                      std::uint64_t logical,
                                                      std::span<std::uint8_t> bitmap)
{
    const std::optional<BlockPath> path = PathOf(logical);
    if (!path)
    {
        return std::unexpected(Error::FileTooLarge);
    }
    std::uint32_t block = inode.Block(path->root);
    for (std::uint32_t level = 0; level < path->depth; ++level)
    {
        MapCursor::Level& held = cursor.levels[level];
        if (block == 0)
        {
            // A new indirect block starts as zeros, all holes, and is linked at once: its parent
            // reaches the disk only after it.
            const Result<std::uint32_t> allocated = AllocateFor(cursor, bitmap);
            if (!allocated)
            {
                return allocated;
            }
            if (const Status released = ReleaseLevels(cursor, level); !released)
            {
                return Failure(released);
            }
            ZeroBytes(held.data);
            held.block = *allocated;
            held.dirty = true;
            if (level == 0)
            {
                inode.SetBlock(path->root, *allocated);
            }
            else
            {
                StorePointer(cursor.levels[level - 1].data, path->index[level - 1], *allocated);
                cursor.levels[level - 1].dirty = true;
            }
        }
        else if (held.block != block)
        {
            if (!IsDataBlock(block))
            {
                return std::unexpected(Error::Corrupt);
            }
            if (const Status released = ReleaseLevels(cursor, level); !released)
            {
                return Failure(released);
            }
            if (const Status read = cache.Read(block, 0, held.data, held.data); !read)
            {
                return Failure(read);
            }
            held.block = block;
            held.dirty = false;
        }
        block = LoadPointer(held.data, path->index[level]);
    }
    if (block != 0 && !IsDataBlock(block))
    {
        return std::unexpected(Error::Corrupt);
    }
    return block;
}

void Ext2Filesystem::LinkBlock(Inode& inode, MapCursor& cursor, std::uint64_t logical,
                               std::uint32_t block)
{
    const std::optional<BlockPath> path = PathOf(logical);
    if (!path)
    {
        return;
    }
    if (path->depth == 0)
    {
        inode.SetBlock(path->root, block);
        return;
    }
    MapCursor::Level& parent = cursor.levels[path->depth - 1];
    StorePointer(parent.data, path->index[path->depth - 1], block);
    parent.dirty = true;
}

Result<std::uint64_t> Ext2Filesystem::FreeTree(std::uint32_t block, std::uint32_t depth,
                                               Buffers& buffers, FreeCursor& frees)
{
    if (!IsDataBlock(block))
    {
        return std::unexpected(Error::Corrupt);
    }
    std::uint64_t count = 0;
    if (depth > 0)
    {
        const std::span<std::uint8_t> pointers = buffers[LevelBuffers + depth - 1];
        if (const Status read = cache.Read(block, 0, pointers, pointers); !read)
        {
            return Failure(read);
        }
        for (std::uint32_t index = 0; index < pointers_per_block; ++index)
        {
            if (const std::uint32_t child = LoadPointer(pointers, index); child != 0)
            {
                const Result<std::uint64_t> freed = FreeTree(child, depth - 1, buffers, frees);
                if (!freed)
                {
                    return freed;
                }
                count += *freed;
            }
        }
    }
    if (const Status freed = FreeBlock(frees, block); !freed)
    {
        return Failure(freed);
    }
    return count + 1;
}

// The indirect block covering first.. at depth, which the new end keep falls inside: it keeps
// what lies before keep and loses the rest, written before the blocks it lost are freed.
Result<std::uint64_t> Ext2Filesystem::ShrinkIndirect(std::uint32_t block, std::uint32_t depth,
                                                     std::uint64_t first, std::uint64_t keep,
                                                     std::uint32_t level, Buffers& buffers,
                                                     FreeCursor& frees)
{
    if (!IsDataBlock(block))
    {
        return std::unexpected(Error::Corrupt);
    }
    const std::span<std::uint8_t> original = buffers[ShrinkOriginals + level];
    const std::span<std::uint8_t> changed = buffers[ShrinkChanged + level];
    if (const Status read = cache.Read(block, 0, original, original); !read)
    {
        return Failure(read);
    }
    CopyBytes(changed.data(), original.data(), block_size);
    const std::uint64_t span = Span(depth - 1, pointers_per_block);
    const std::uint64_t into = keep - first;
    const auto straddling = static_cast<std::uint32_t>(into / span);
    const std::uint32_t first_cut = into % span == 0 ? straddling : straddling + 1;
    std::uint64_t freed = 0;
    if (first_cut != straddling)
    {
        if (const std::uint32_t child = LoadPointer(original, straddling); child != 0)
        {
            const Result<std::uint64_t> count = ShrinkIndirect(
                child, depth - 1, first + straddling * span, keep, level + 1, buffers, frees);
            if (!count)
            {
                return count;
            }
            freed += *count;
        }
    }
    bool cut = false;
    for (std::uint32_t index = first_cut; index < pointers_per_block; ++index)
    {
        if (LoadPointer(original, index) != 0)
        {
            StorePointer(changed, index, 0);
            cut = true;
        }
    }
    if (!cut)
    {
        return freed;
    }
    if (const Status written = cache.Write(block, changed); !written)
    {
        return Failure(written);
    }
    for (std::uint32_t index = first_cut; index < pointers_per_block; ++index)
    {
        if (const std::uint32_t child = LoadPointer(original, index); child != 0)
        {
            const Result<std::uint64_t> count = FreeTree(child, depth - 1, buffers, frees);
            if (!count)
            {
                return count;
            }
            freed += *count;
        }
    }
    return freed;
}

Status Ext2Filesystem::ShrinkBlocks(NodeId node, Inode& inode, std::uint64_t size, Buffers& buffers)
{
    const std::span<std::uint8_t> scratch = buffers[ScratchBuffer];
    if (const Status zeroed = ZeroFrom(inode, size, scratch); !zeroed)
    {
        return zeroed;
    }
    const std::uint64_t keep = DivideRoundingUp(size, block_size);
    const std::uint64_t per_block = pointers_per_block;
    FreeCursor frees = {
        .bitmap = buffers[BitmapBuffer], .group = 0, .loaded = false, .dirty = false};
    Status status;
    std::uint64_t freed = 0;
    // At most one indirect tree reaches over the new end; it keeps its blocks before it.
    for (std::uint32_t root = SingleIndirectBlock; root < BlockPointers && status; ++root)
    {
        const std::uint64_t first = RootFirst(root, per_block);
        const std::uint32_t depth = RootDepth(root);
        const std::uint32_t block = inode.Block(root);
        if (block != 0 && first < keep && keep < first + Span(depth, per_block))
        {
            const Result<std::uint64_t> count =
                ShrinkIndirect(block, depth, first, keep, 0, buffers, frees);
            if (count)
            {
                freed += *count;
            }
            else
            {
                status = Failure(count);
            }
        }
    }
    // Trees wholly past the end leave the inode, which is written before they are freed.
    std::array<std::uint32_t, BlockPointers> detached{};
    if (status)
    {
        for (std::uint32_t root = 0; root < BlockPointers; ++root)
        {
            if (RootFirst(root, per_block) >= keep)
            {
                detached[root] = inode.Block(root);
                inode.SetBlock(root, 0);
            }
        }
        const Timestamp now = Now();
        inode.size = size;
        inode.modify_time = now;
        inode.change_time = now;
        ReleaseBytes(inode, freed * block_size);
        status = StoreInode(node, inode, false, scratch);
    }
    std::uint64_t later = 0;
    for (std::uint32_t root = 0; root < BlockPointers && status; ++root)
    {
        if (detached[root] != 0)
        {
            const Result<std::uint64_t> count =
                FreeTree(detached[root], RootDepth(root), buffers, frees);
            if (count)
            {
                later += *count;
            }
            else
            {
                status = Failure(count);
            }
        }
    }
    const Status finished = FinishFrees(frees);
    if (!status)
    {
        return status;
    }
    if (!finished)
    {
        return finished;
    }
    if (later == 0)
    {
        return {};
    }
    ReleaseBytes(inode, later * block_size);
    return StoreInode(node, inode, false, scratch);
}

Status Ext2Filesystem::ReleaseAttributeBlock(std::uint32_t block, FreeCursor& frees,
                                             std::span<std::uint8_t> scratch)
{
    if (!IsDataBlock(block))
    {
        return std::unexpected(Error::Corrupt);
    }
    if (const Status read = cache.Read(block, 0, scratch, scratch); !read)
    {
        return read;
    }
    if (!AttributeBlockMatches(block, scratch))
    {
        return std::unexpected(Error::Corrupt);
    }
    const std::uint32_t references =
        LoadLittleEndian<std::uint32_t>(scratch.data() + AttributeReferences);
    if (references > 1)
    {
        StoreLittleEndian(scratch.data() + AttributeReferences, references - 1);
        if (metadata_checksums)
        {
            StoreLittleEndian(scratch.data() + AttributeChecksum,
                              AttributeBlockChecksum(volume_seed, block, scratch));
        }
        return cache.Write(block, scratch);
    }
    return FreeBlock(frees, block);
}

bool Ext2Filesystem::AttributeBlockMatches(std::uint32_t block,
                                           std::span<const std::uint8_t> contents) const
{
    return LoadLittleEndian<std::uint32_t>(contents.data()) == AttributeMagic &&
           LoadLittleEndian<std::uint32_t>(contents.data() + AttributeBlocks) == 1 &&
           (!metadata_checksums ||
            LoadLittleEndian<std::uint32_t>(contents.data() + AttributeChecksum) ==
                AttributeBlockChecksum(volume_seed, block, contents));
}

Status Ext2Filesystem::CheckReleasable(const Inode& inode, std::span<std::uint8_t> scratch)
{
    if ((inode.flags & (InodeExtents | InodeInlineData)) != 0)
    {
        return std::unexpected(Error::Unsupported);
    }
    // A block past 2^32 on a 64bit volume, which FreeBlock cannot take.
    if ((superblock.incompatible & Incompatible64Bit) != 0 && inode.attribute_block_high != 0)
    {
        return std::unexpected(Error::Unsupported);
    }
    if (inode.attribute_block == 0)
    {
        return {};
    }
    if (!IsDataBlock(inode.attribute_block))
    {
        return std::unexpected(Error::Corrupt);
    }
    if (const Status read = cache.Read(inode.attribute_block, 0, scratch, scratch); !read)
    {
        return read;
    }
    if (!AttributeBlockMatches(inode.attribute_block, scratch))
    {
        return std::unexpected(Error::Corrupt);
    }
    return {};
}

// The blocks of an inode that is already marked deleted on the volume.
Status Ext2Filesystem::ReleaseContents(const Node& node, Buffers& buffers)
{
    const Inode& inode = node.inode;
    FreeCursor frees = {
        .bitmap = buffers[BitmapBuffer], .group = 0, .loaded = false, .dirty = false};
    // A fast symbolic link keeps its target in i_block and a device its numbers, so i_block holds
    // block numbers only for a file, a directory and a link with a block of its own, which
    // i_blocks tells apart, as on Linux.
    const std::uint64_t attribute_bytes = inode.attribute_block != 0 ? block_size : 0;
    const bool has_blocks =
        node.type == NodeType::File || node.type == NodeType::Directory ||
        (node.type == NodeType::SymbolicLink && inode.allocated_bytes > attribute_bytes);
    Status status;
    for (std::uint32_t root = 0; root < BlockPointers && has_blocks && status; ++root)
    {
        if (const std::uint32_t block = inode.Block(root); block != 0)
        {
            const Result<std::uint64_t> freed = FreeTree(block, RootDepth(root), buffers, frees);
            if (!freed)
            {
                status = Failure(freed);
            }
        }
    }
    if (status && inode.attribute_block != 0)
    {
        status = ReleaseAttributeBlock(inode.attribute_block, frees, buffers[ScratchBuffer]);
    }
    const Status finished = FinishFrees(frees);
    return !status ? status : finished;
}

Status Ext2Filesystem::AddEntry(NodeId directory, Inode& inode, std::optional<RecordPlace> room,
                                const RawEntry& entry, Buffers& buffers)
{
    const std::span<std::uint8_t> block = buffers[DataBuffer];
    const std::span<std::uint8_t> scratch = buffers[ScratchBuffer];
    RawEntry placed = entry;
    if (room)
    {
        // The new record takes a record's slack, or a whole unused record.
        if (const Status read = cache.Read(room->physical, 0, block, block); !read)
        {
            return read;
        }
        const Result<RawEntry> record =
            ParseEntry(block, room->offset, has_file_type, superblock.inodes_count);
        if (!record)
        {
            return Failure(record);
        }
        const std::size_t used = record->inode != 0 ? RecordLength(record->name.size()) : 0;
        if (record->record_length - used < RecordLength(entry.name.size()))
        {
            return std::unexpected(Error::Corrupt);
        }
        placed.record_length = static_cast<std::uint16_t>(record->record_length - used);
        if (used != 0)
        {
            StoreLittleEndian(block.data() + room->offset + 4, static_cast<std::uint16_t>(used));
        }
        StoreEntry(block, room->offset + used, placed, has_file_type);
        if (const Status written = WriteDirectoryBlock(inode, room->logical, room->physical, block);
            !written)
        {
            return written;
        }
    }
    else
    {
        // A new block holding only this record, written before the directory takes it in.
        if (inode.size + block_size > MaximumDirectoryBytes)
        {
            return std::unexpected(Error::NoSpace);
        }
        const std::uint64_t logical = inode.size / block_size;
        MapCursor cursor = {
            .levels = {{{.block = 0, .dirty = false, .data = buffers[LevelBuffers]},
                        {.block = 0, .dirty = false, .data = buffers[LevelBuffers + 1]},
                        {.block = 0, .dirty = false, .data = buffers[LevelBuffers + 2]}}},
            .goal = static_cast<std::uint32_t>(GroupFirstBlock(GroupOfInode(directory))),
            .allocated = 0,
        };
        if (logical > 0)
        {
            if (const Result<std::uint32_t> last = MapBlock(inode, logical - 1, scratch);
                last && *last != 0)
            {
                cursor.goal = *last + 1;
            }
        }
        Status status;
        const Result<std::uint32_t> existing =
            ResolveForWrite(inode, cursor, logical, buffers[BitmapBuffer]);
        if (!existing)
        {
            status = Failure(existing);
        }
        else if (*existing != 0)
        {
            // A block past the directory's end.
            status = std::unexpected(Error::Corrupt);
        }
        else if (const Result<std::uint32_t> fresh = AllocateFor(cursor, buffers[BitmapBuffer]);
                 !fresh)
        {
            status = Failure(fresh);
        }
        else
        {
            ZeroBytes(block);
            placed.record_length = static_cast<std::uint16_t>(
                metadata_checksums ? block_size - DirectoryTailSize : block_size);
            StoreEntry(block, 0, placed, has_file_type);
            status = WriteDirectoryBlock(inode, logical, *fresh, block);
            if (status)
            {
                LinkBlock(inode, cursor, logical, *fresh);
            }
        }
        if (const Status flushed = ReleaseLevels(cursor, 0); !flushed)
        {
            return flushed;
        }
        inode.allocated_bytes += cursor.allocated * block_size;
        if (!status)
        {
            // An indirect block may have joined the directory all the same.
            if (cursor.allocated > 0)
            {
                [[maybe_unused]] const Status stored = StoreInode(directory, inode, false, scratch);
            }
            return status;
        }
        inode.size += block_size;
    }
    const Timestamp now = Now();
    inode.modify_time = now;
    inode.change_time = now;
    return StoreInode(directory, inode, false, scratch);
}

Result<std::size_t> Ext2Filesystem::WriteFile(NodeId file, std::uint64_t offset,
                                              std::span<const std::uint8_t> data)
{
    if (IsReserved(file))
    {
        return std::unexpected(Error::InvalidArgument);
    }
    Buffers buffers(block_size, ChangeBuffers);
    if (buffers.Empty())
    {
        return std::unexpected(Error::OutOfMemory);
    }
    const std::span<std::uint8_t> scratch = buffers[ScratchBuffer];
    const Result<Node> node = LoadNode(file, scratch);
    if (!node)
    {
        return Failure(node);
    }
    if (node->type == NodeType::Directory)
    {
        return std::unexpected(Error::IsDirectory);
    }
    if (node->type != NodeType::File)
    {
        return std::unexpected(Error::InvalidArgument);
    }
    Inode inode = node->inode;
    if ((inode.flags & (InodeExtents | InodeInlineData)) != 0)
    {
        return std::unexpected(Error::Unsupported);
    }
    if (data.empty())
    {
        return std::size_t{0};
    }
    const std::uint64_t maximum = MaximumFileSize();
    if (offset >= maximum)
    {
        return std::unexpected(Error::FileTooLarge);
    }
    if (data.size() > maximum - offset)
    {
        data = data.first(static_cast<std::size_t>(maximum - offset));
    }
    if (const Status begun = BeginChange(scratch); !begun)
    {
        return Failure(begun);
    }
    if (offset > inode.size)
    {
        if (const Status zeroed = ZeroFrom(inode, inode.size, scratch); !zeroed)
        {
            return Failure(zeroed);
        }
    }

    // Blocks go after the file's block before the write, or at the start of its inode's group.
    MapCursor cursor = {
        .levels = {{{.block = 0, .dirty = false, .data = buffers[LevelBuffers]},
                    {.block = 0, .dirty = false, .data = buffers[LevelBuffers + 1]},
                    {.block = 0, .dirty = false, .data = buffers[LevelBuffers + 2]}}},
        .goal = static_cast<std::uint32_t>(GroupFirstBlock(GroupOfInode(file))),
        .allocated = 0,
    };
    if (offset / block_size > 0)
    {
        if (const Result<std::uint32_t> before = MapBlock(inode, offset / block_size - 1, scratch);
            before && *before != 0)
        {
            cursor.goal = *before + 1;
        }
    }
    const std::span<std::uint8_t> block = buffers[DataBuffer];
    const std::span<std::uint8_t> bitmap = buffers[BitmapBuffer];
    std::size_t written = 0;
    Status failure;
    while (written < data.size())
    {
        const std::uint64_t position = offset + written;
        const std::uint64_t logical = position / block_size;
        const auto within = static_cast<std::size_t>(position % block_size);
        const std::size_t part = block_size - within < data.size() - written
                                     ? block_size - within
                                     : data.size() - written;
        const std::span<const std::uint8_t> chunk = data.subspan(written, part);
        const Result<std::uint32_t> mapped = ResolveForWrite(inode, cursor, logical, bitmap);
        if (!mapped)
        {
            failure = Failure(mapped);
            break;
        }
        std::uint32_t physical = *mapped;
        if (physical == 0)
        {
            // A hole gets a block of zeros around the new bytes, linked only once it is written.
            const Result<std::uint32_t> fresh = AllocateFor(cursor, bitmap);
            if (!fresh)
            {
                failure = Failure(fresh);
                break;
            }
            ZeroBytes(block);
            CopyBytes(block.data() + within, chunk.data(), part);
            failure = cache.Write(*fresh, block);
            if (!failure)
            {
                break;
            }
            LinkBlock(inode, cursor, logical, *fresh);
            physical = *fresh;
        }
        else if (part == block_size)
        {
            failure = cache.Write(physical, chunk);
        }
        else
        {
            failure = cache.ReadUncached(physical, block);
            if (failure)
            {
                CopyBytes(block.data() + within, chunk.data(), part);
                failure = cache.Write(physical, block);
            }
        }
        if (!failure)
        {
            break;
        }
        written += part;
        cursor.goal = physical + 1;
    }

    const Status flushed = ReleaseLevels(cursor, 0);
    if (written == 0 && cursor.allocated == 0)
    {
        return Failure(failure);
    }
    if (!flushed)
    {
        return Failure(flushed);
    }
    inode.allocated_bytes += cursor.allocated * block_size;
    if (written > 0)
    {
        if (offset + written > inode.size)
        {
            inode.size = offset + written;
        }
        const Timestamp now = Now();
        inode.modify_time = now;
        inode.change_time = now;
    }
    if (const Status large = SetLargeFile(inode.size, scratch); !large)
    {
        return Failure(large);
    }
    if (const Status stored = StoreInode(file, inode, false, scratch); !stored)
    {
        return Failure(stored);
    }
    if (written == 0)
    {
        return Failure(failure);
    }
    return written;
}

Status Ext2Filesystem::TruncateFile(NodeId file, std::uint64_t size)
{
    if (IsReserved(file))
    {
        return std::unexpected(Error::InvalidArgument);
    }
    Buffers buffers(block_size, ShrinkBuffers);
    if (buffers.Empty())
    {
        return std::unexpected(Error::OutOfMemory);
    }
    const std::span<std::uint8_t> scratch = buffers[ScratchBuffer];
    const Result<Node> node = LoadNode(file, scratch);
    if (!node)
    {
        return Failure(node);
    }
    if (node->type == NodeType::Directory)
    {
        return std::unexpected(Error::IsDirectory);
    }
    if (node->type != NodeType::File)
    {
        return std::unexpected(Error::InvalidArgument);
    }
    Inode inode = node->inode;
    if ((inode.flags & (InodeExtents | InodeInlineData)) != 0)
    {
        return std::unexpected(Error::Unsupported);
    }
    if (size > MaximumFileSize())
    {
        return std::unexpected(Error::FileTooLarge);
    }
    if (size == inode.size)
    {
        return {};
    }
    if (const Status begun = BeginChange(scratch); !begun)
    {
        return begun;
    }
    if (size < inode.size)
    {
        return ShrinkBlocks(file, inode, size, buffers);
    }
    // Growing leaves a hole: nothing is allocated.
    if (const Status zeroed = ZeroFrom(inode, inode.size, scratch); !zeroed)
    {
        return zeroed;
    }
    const Timestamp now = Now();
    inode.size = size;
    inode.modify_time = now;
    inode.change_time = now;
    if (const Status large = SetLargeFile(size, scratch); !large)
    {
        return large;
    }
    return StoreInode(file, inode, false, scratch);
}

Status Ext2Filesystem::ChangeAttributes(NodeId node, const AttributeChanges& changes)
{
    if (IsReserved(node))
    {
        return std::unexpected(Error::InvalidArgument);
    }
    if ((changes.access_time && changes.access_time->nanoseconds > MaximumNanoseconds) ||
        (changes.modify_time && changes.modify_time->nanoseconds > MaximumNanoseconds))
    {
        return std::unexpected(Error::InvalidArgument);
    }
    if (!changes.mode && !changes.owner && !changes.group && !changes.access_time &&
        !changes.modify_time)
    {
        return {};
    }
    Buffers buffers(block_size, 1);
    if (buffers.Empty())
    {
        return std::unexpected(Error::OutOfMemory);
    }
    const Result<Node> loaded = LoadNode(node, buffers[0]);
    if (!loaded)
    {
        return Failure(loaded);
    }
    if (const Status begun = BeginChange(buffers[0]); !begun)
    {
        return begun;
    }
    Inode inode = loaded->inode;
    if (changes.mode)
    {
        inode.mode = static_cast<std::uint16_t>((inode.mode & ModeTypeMask) |
                                                (*changes.mode & ModePermissions));
    }
    if (changes.owner)
    {
        inode.owner = *changes.owner;
    }
    if (changes.group)
    {
        inode.group = *changes.group;
    }
    if (changes.access_time)
    {
        inode.access_time = *changes.access_time;
    }
    if (changes.modify_time)
    {
        inode.modify_time = *changes.modify_time;
    }
    inode.change_time = Now();
    return StoreInode(node, inode, false, buffers[0]);
}

Status Ext2Filesystem::SetAttributes(NodeId node, const AttributeChanges& changes)
{
    if (read_only)
    {
        return Refused();
    }
    return Noted(ChangeAttributes(node, changes));
}

Result<std::size_t> Ext2Filesystem::Write(NodeId file, std::uint64_t offset,
                                          std::span<const std::uint8_t> data)
{
    if (read_only)
    {
        return Refused();
    }
    return Noted(WriteFile(file, offset, data));
}

Status Ext2Filesystem::Truncate(NodeId file, std::uint64_t size)
{
    if (read_only)
    {
        return Refused();
    }
    return Noted(TruncateFile(file, size));
}

Status Ext2Filesystem::Sync()
{
    if (!writable_mount)
    {
        return {};
    }
    if (!superblock_clean)
    {
        Buffers buffers(block_size, 1);
        if (buffers.Empty())
        {
            return std::unexpected(Error::OutOfMemory);
        }
        if (const Status written = WriteGroups(); !written)
        {
            return written;
        }
        // Everything else is on the disk before the superblock says the volume is clean.
        if (const Status flushed = volume.Flush(); !flushed)
        {
            return flushed;
        }
        superblock.write_time = static_cast<std::uint32_t>(Now().seconds);
        superblock.state = static_cast<std::uint16_t>(superblock.state | StateClean);
        if (const Status written = WriteSuperblock(buffers[0]); !written)
        {
            return written;
        }
        superblock_clean = true;
    }
    return volume.Flush();
}

} // namespace FeatherDisk::Ext
