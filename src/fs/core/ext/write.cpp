// SPDX-License-Identifier: MIT OR Apache-2.0
// Changes to an ext2 volume. Without a journal, the order of the writes is what keeps a volume
// whole across a crash:
//   - a block or inode is marked in its bitmap before anything points to it;
//   - a new block's contents (file data, a zeroed indirect block, a directory block) are written
//     before the pointer to it, indirect blocks before the inode that holds them, and an inode
//     before the directory entry that names it;
//   - removing runs the other way: the entry goes first, then the inode's link, and the blocks
//     and the inode go back to their bitmaps last.
// A crash can then leave leaked blocks and inodes, free counts, sizes and i_blocks that lag, an
// unattached inode or a link count one too high, all of which e2fsck repairs in preen mode, but
// never a pointer to a block or an inode that is free or not yet written. The volume is marked
// not clean before the first change, so after a crash it mounts read-only until e2fsck has run.
// The order is that of BlockDevice writes, which reach the device at once (write-through); only
// Sync's Flush is a barrier, so a device that reorders completed writes in a volatile cache can
// still break it on power loss. The group descriptors and the superblock's counts are kept in
// memory and written by Sync.
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
// What Linux and mke2fs give a new large inode: room for the extended times and creation time.
constexpr std::uint16_t NewInodeExtraSize = 32;
// The header of an extended attribute block: h_magic, then h_refcount, the inodes sharing it.
constexpr std::uint32_t AttributeMagic = 0xEA020000;
constexpr std::size_t AttributeReferences = 4;

// The buffers of one change, by use.
constexpr std::size_t DataBuffer = 0;
constexpr std::size_t ScratchBuffer = 1;
constexpr std::size_t BitmapBuffer = 2;
// Three: the indirect blocks a MapCursor holds, or those FreeTree walks.
constexpr std::size_t LevelBuffers = 3;
constexpr std::size_t ChangeBuffers = 6;
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

Status CheckName(std::string_view name)
{
    if (name.size() > MaximumNameLength)
    {
        return std::unexpected(Error::NameTooLong);
    }
    if (name.empty() || name.find('/') != std::string_view::npos ||
        name.find('\0') != std::string_view::npos)
    {
        return std::unexpected(Error::InvalidArgument);
    }
    return {};
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
    EncodeSuperblockState(superblock, scratch.subspan(offset).first<SuperblockSize>());
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
    superblock.free_blocks_count = static_cast<std::uint32_t>(free_blocks);
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
    if (superblock.errors == ErrorsContinue)
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
    EncodeInode(inode, slot.first(superblock.inode_size < DecodedInodeBytes ? superblock.inode_size
                                                                            : DecodedInodeBytes));
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

// A changed directory no longer matches a hash index, so it drops the index before any of its
// blocks change, as Linux's ext2 does; its blocks still read as a plain directory.
Status Ext2Filesystem::ClearIndex(NodeId directory, Inode& inode, std::span<std::uint8_t> scratch)
{
    if ((inode.flags & InodeIndexed) == 0)
    {
        return {};
    }
    inode.flags &= ~InodeIndexed;
    return StoreInode(directory, inode, false, scratch);
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
    if (LoadLittleEndian<std::uint32_t>(scratch.data()) != AttributeMagic)
    {
        return std::unexpected(Error::Corrupt);
    }
    const std::uint32_t references =
        LoadLittleEndian<std::uint32_t>(scratch.data() + AttributeReferences);
    if (references > 1)
    {
        StoreLittleEndian(scratch.data() + AttributeReferences, references - 1);
        return cache.Write(block, scratch);
    }
    return FreeBlock(frees, block);
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
        if (const Status written = cache.Write(room->physical, block); !written)
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
            placed.record_length = static_cast<std::uint16_t>(block_size);
            StoreEntry(block, 0, placed, has_file_type);
            status = cache.Write(*fresh, block);
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

Result<NodeId> Ext2Filesystem::CreateFile(NodeId directory, std::string_view name,
                                          std::uint32_t mode)
{
    if (const Status valid = CheckName(name); !valid)
    {
        return Failure(valid);
    }
    if (name == "." || name == "..")
    {
        return std::unexpected(Error::Exists);
    }
    Buffers buffers(block_size, ChangeBuffers);
    if (buffers.Empty())
    {
        return std::unexpected(Error::OutOfMemory);
    }
    const std::span<std::uint8_t> scratch = buffers[ScratchBuffer];
    const Result<Node> parent = LoadNode(directory, scratch);
    if (!parent)
    {
        return Failure(parent);
    }
    if (parent->type != NodeType::Directory)
    {
        return std::unexpected(Error::NotDirectory);
    }
    // One walk checks the name is new and finds the first record with room after it.
    const std::size_t needed = RecordLength(name.size());
    std::optional<RecordPlace> room;
    bool exists = false;
    const Result<bool> walked =
        WalkDirectory(parent->inode, 0, buffers[DataBuffer], scratch,
                      [&](const RawEntry& entry, const RecordPlace& place) -> Result<bool>
                      {
                          if (entry.inode != 0 && entry.name == name)
                          {
                              exists = true;
                              return true;
                          }
                          const std::size_t used =
                              entry.inode != 0 ? RecordLength(entry.name.size()) : 0;
                          if (!room && entry.record_length - used >= needed)
                          {
                              room = place;
                          }
                          return false;
                      });
    if (!walked)
    {
        return Failure(walked);
    }
    if (exists)
    {
        return std::unexpected(Error::Exists);
    }
    if (const Status begun = BeginChange(scratch); !begun)
    {
        return Failure(begun);
    }
    Inode directory_inode = parent->inode;
    if (const Status cleared = ClearIndex(directory, directory_inode, scratch); !cleared)
    {
        return Failure(cleared);
    }
    const Result<NodeId> node =
        AllocateInode(GroupOfInode(directory), false, buffers[BitmapBuffer], scratch);
    if (!node)
    {
        return node;
    }

    const Timestamp now = Now();
    std::uint16_t extra_size = 0;
    if (superblock.inode_size > GoodOldInodeSize)
    {
        // At least what the volume asks for, within the inode.
        std::uint32_t wanted = NewInodeExtraSize;
        wanted = superblock.minimum_extra_size > wanted ? superblock.minimum_extra_size : wanted;
        wanted = superblock.wanted_extra_size > wanted ? superblock.wanted_extra_size : wanted;
        const std::uint32_t room_left = superblock.inode_size - GoodOldInodeSize;
        extra_size = static_cast<std::uint16_t>((wanted < room_left ? wanted : room_left) & ~3U);
    }
    Inode inode = {
        .mode = static_cast<std::uint16_t>(ModeRegular | (mode & ModePermissions)),
        .owner = 0,
        .group = 0,
        .size = 0,
        .link_count = 1,
        .flags = 0,
        .allocated_bytes = 0,
        .access_time = now,
        .modify_time = now,
        .change_time = now,
        .blocks = {},
        .deletion_time = 0,
        .attribute_block = 0,
        .extra_size = extra_size,
        .creation_time = now,
    };
    if (const Status stored = StoreInode(*node, inode, true, scratch); !stored)
    {
        return Failure(stored);
    }
    const RawEntry entry = {
        .inode = static_cast<std::uint32_t>(*node),
        .record_length = 0,
        .file_type = has_file_type ? EntryFileType(inode.mode) : std::uint8_t{0},
        .name = name,
    };
    if (const Status added = AddEntry(directory, directory_inode, room, entry, buffers); !added)
    {
        // Nothing names the inode, so it goes back, deleted before it leaves the bitmap.
        inode.link_count = 0;
        inode.deletion_time = static_cast<std::uint32_t>(now.seconds);
        if (StoreInode(*node, inode, false, scratch))
        {
            // If this fails too, the inode only leaks.
            [[maybe_unused]] const Status freed = FreeInode(*node, false, buffers[BitmapBuffer]);
        }
        return Failure(added);
    }
    return node;
}

Status Ext2Filesystem::RemoveFile(NodeId directory, std::string_view name)
{
    if (name == "." || name == "..")
    {
        return std::unexpected(Error::IsDirectory);
    }
    if (const Status valid = CheckName(name); !valid)
    {
        return valid;
    }
    Buffers buffers(block_size, ChangeBuffers);
    if (buffers.Empty())
    {
        return std::unexpected(Error::OutOfMemory);
    }
    const std::span<std::uint8_t> block = buffers[DataBuffer];
    const std::span<std::uint8_t> scratch = buffers[ScratchBuffer];
    const Result<Node> parent = LoadNode(directory, scratch);
    if (!parent)
    {
        return Failure(parent);
    }
    if (parent->type != NodeType::Directory)
    {
        return std::unexpected(Error::NotDirectory);
    }
    std::optional<RecordPlace> place;
    std::uint32_t target = 0;
    std::uint16_t record_length = 0;
    const Result<bool> walked =
        WalkDirectory(parent->inode, 0, block, scratch,
                      [&](const RawEntry& entry, const RecordPlace& where) -> Result<bool>
                      {
                          if (entry.inode == 0 || entry.name != name)
                          {
                              return false;
                          }
                          place = where;
                          target = entry.inode;
                          record_length = entry.record_length;
                          return true;
                      });
    if (!walked)
    {
        return Failure(walked);
    }
    if (!*walked || !place)
    {
        return std::unexpected(Error::NotFound);
    }
    if (IsReserved(target))
    {
        return std::unexpected(Error::InvalidArgument);
    }
    const Result<Node> node = LoadNode(target, scratch);
    if (!node)
    {
        // An entry naming a free inode.
        return std::unexpected(node.error() == Error::NotFound ? Error::Corrupt : node.error());
    }
    if (node->type == NodeType::Directory)
    {
        return std::unexpected(Error::IsDirectory);
    }
    Inode inode = node->inode;
    const bool last_link = inode.link_count <= 1;
    if (last_link)
    {
        // Whatever the last link frees is checked before anything changes.
        if ((inode.flags & (InodeExtents | InodeInlineData)) != 0)
        {
            return std::unexpected(Error::Unsupported);
        }
        if (inode.attribute_block != 0)
        {
            const std::span<std::uint8_t> attributes = buffers[BitmapBuffer];
            if (!IsDataBlock(inode.attribute_block))
            {
                return std::unexpected(Error::Corrupt);
            }
            if (const Status read = cache.Read(inode.attribute_block, 0, attributes, attributes);
                !read)
            {
                return read;
            }
            if (LoadLittleEndian<std::uint32_t>(attributes.data()) != AttributeMagic)
            {
                return std::unexpected(Error::Corrupt);
            }
        }
    }
    if (const Status begun = BeginChange(scratch); !begun)
    {
        return begun;
    }
    Inode directory_inode = parent->inode;
    if (const Status cleared = ClearIndex(directory, directory_inode, scratch); !cleared)
    {
        return cleared;
    }
    // The record before takes over the space, or the block's first record becomes unused.
    if (place->previous != NoRecord)
    {
        std::uint8_t* previous = block.data() + place->previous + 4;
        StoreLittleEndian(previous, static_cast<std::uint16_t>(
                                        LoadLittleEndian<std::uint16_t>(previous) + record_length));
    }
    StoreLittleEndian(block.data() + place->offset, std::uint32_t{0});
    if (const Status written = cache.Write(place->physical, block); !written)
    {
        return written;
    }
    const Timestamp now = Now();
    directory_inode.modify_time = now;
    directory_inode.change_time = now;
    if (const Status stored = StoreInode(directory, directory_inode, false, scratch); !stored)
    {
        return stored;
    }
    inode.change_time = now;
    if (!last_link)
    {
        --inode.link_count;
        return StoreInode(target, inode, false, scratch);
    }
    // With no links and a deletion time, e2fsck counts the inode free whatever it still points
    // to, so it is written before its blocks and itself go back to the bitmaps.
    inode.link_count = 0;
    inode.deletion_time = static_cast<std::uint32_t>(now.seconds);
    if (const Status stored = StoreInode(target, inode, false, scratch); !stored)
    {
        return stored;
    }
    if (const Status released = ReleaseContents({.inode = inode, .type = node->type}, buffers);
        !released)
    {
        return released;
    }
    return FreeInode(target, false, buffers[BitmapBuffer]);
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

Result<NodeId> Ext2Filesystem::Create(NodeId directory, std::string_view name, std::uint32_t mode)
{
    if (read_only)
    {
        return Refused();
    }
    return Noted(CreateFile(directory, name, mode));
}

Status Ext2Filesystem::Remove(NodeId directory, std::string_view name)
{
    if (read_only)
    {
        return Refused();
    }
    return Noted(RemoveFile(directory, name));
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
