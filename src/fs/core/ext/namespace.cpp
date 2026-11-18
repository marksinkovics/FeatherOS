// SPDX-License-Identifier: MIT OR Apache-2.0
// Changes to an ext2 volume's directory tree: creating and removing files, directories and links,
// and renaming. They keep the write order stated at the top of write.cpp, so a crash leaves only
// what e2fsck repairs. What that means for each operation:
//   - mkdir: the inode and its block in their bitmaps, the block with "." and "..", the parent's
//     link count (the new ".." links it as soon as the inode is in use), the new inode with two
//     links, then the entry. A crash leaves at worst an unconnected directory and the parent's
//     count one too high, never too low.
//   - rmdir: the entry, the inode marked deleted (which ends its ".." link), the parent's count,
//     then its blocks and the inode freed. Removed entries never shrink a directory, as in Linux's
//     ext2: their records are merged into the one before or marked unused, and the blocks stay
//     for later entries.
//   - link: the count goes up, then the entry, so a crash leaves the count one too high.
//   - rename, S named from_name in P1 moving to to_name in P2, where T may already be:
//       1. a directory S with a new parent: P2's count up (for S's "..");
//       2. to_name names S: T's record pointed at S in place, or a new record; P2's times;
//       3. from_name's record dropped; P1's times;
//       4. a directory S with a new parent: S's ".." pointed at P2, then P1's count down;
//       5. S's change time;
//       6. T loses its link, freed with the last one as by unlink or rmdir (a directory T's ".."
//          leaving P2's count once T is marked deleted).
//     After 1, P2's count is one too high. Between 2 and 3, S has both names, and its count is
//     one too low: e2fsck raises a file's count, and for a directory reports the second name as
//     a link to a directory and clears one of the two. T, unnamed until 6, is an unattached inode
//     or an unconnected directory, whose blocks are kept until e2fsck has run. Between 3 and 4, a
//     moved directory's ".." still names P1, which e2fsck points at P2. Adding before removing
//     means a crash never loses S, at the price of those repairs.
//   - symlink: as a file, with a target shorter than i_block stored in the inode ("fast", no
//     blocks) and a longer one in a block of its own, written before the inode.
// A hashed directory keeps its index while names leave it or an entry is pointed elsewhere, and
// loses it before a name is added (ClearIndex in write.cpp).
#include "common/bytes.h"
#include "common/little_endian.h"
#include "ext/ext2_filesystem.h"
#include "ext/extent.h"

namespace FeatherDisk::Ext
{
namespace
{

// EXT2_LINK_MAX: Linux's ext2 allows no more links to an inode, so no more subdirectories either.
constexpr std::uint16_t MaximumLinks = 32000;
// EXT4_LINK_MAX, for volumes with dir_nlink.
constexpr std::uint16_t MaximumExt4Links = 65000;
// What Linux and mke2fs give a new large inode: room for the extended times and creation time.
constexpr std::uint16_t NewInodeExtraSize = 32;
constexpr std::uint16_t LinkPermissions = 0777;
// "." takes the first record of a new directory's block, ".." the rest.
constexpr std::uint16_t DotRecordLength = 12;

bool IsDots(std::string_view name)
{
    return name == "." || name == "..";
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

// An entry naming an inode that is free.
Error Dangling(Error error)
{
    return error == Error::NotFound ? Error::Corrupt : error;
}

} // namespace

Inode Ext2Filesystem::NewInode(std::uint16_t mode, Timestamp now) const
{
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
    return {
        .mode = mode,
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
        .attribute_block_high = 0,
        .extra_size = extra_size,
        .creation_time = now,
        .generation = 0,
        .checksum_seed = 0,
    };
}

Result<NameSearch> Ext2Filesystem::SearchDirectory(const Inode& directory, std::string_view name,
                                                   std::size_t needed, Buffers& buffers)
{
    NameSearch search = {.place = std::nullopt, .inode = 0, .file_type = 0, .room = std::nullopt};
    const Result<bool> walked =
        WalkDirectory(directory, 0, buffers[DataBuffer], buffers[ScratchBuffer],
                      [&](const RawEntry& entry, const RecordPlace& place) -> Result<bool>
                      {
                          if (entry.inode != 0 && entry.name == name)
                          {
                              search.place = place;
                              search.inode = entry.inode;
                              search.file_type = entry.file_type;
                              return true;
                          }
                          // An index block's records only hide the index; they are no room.
                          const std::size_t used =
                              entry.inode != 0 ? RecordLength(entry.name.size()) : 0;
                          if (!search.room && !place.index && entry.record_length - used >= needed)
                          {
                              search.room = place;
                          }
                          return false;
                      });
    if (!walked)
    {
        return Failure(walked);
    }
    return search;
}

// The record before takes over the space, or the block's first record becomes unused, so the
// block stays covered by records.
Status Ext2Filesystem::DropRecord(const Inode& directory, const RecordPlace& place,
                                  std::uint32_t inode, std::span<std::uint8_t> block)
{
    if (const Status read = ReadMetadata(place.physical, 0, block, block); !read)
    {
        return read;
    }
    const Result<RawEntry> record =
        ParseEntry(block, place.offset, has_file_type, superblock.inodes_count);
    if (!record)
    {
        return Failure(record);
    }
    if (record->inode != inode)
    {
        return std::unexpected(Error::Corrupt);
    }
    if (place.previous != NoRecord)
    {
        const Result<RawEntry> previous =
            ParseEntry(block, place.previous, has_file_type, superblock.inodes_count);
        if (!previous)
        {
            return Failure(previous);
        }
        if (place.previous + previous->record_length != place.offset)
        {
            return std::unexpected(Error::Corrupt);
        }
        StoreLittleEndian(
            block.data() + place.previous + 4,
            static_cast<std::uint16_t>(previous->record_length + record->record_length));
    }
    StoreLittleEndian(block.data() + place.offset, std::uint32_t{0});
    return WriteDirectoryBlock(directory, place.logical, place.physical, block);
}

Status Ext2Filesystem::RewriteRecord(const Inode& directory, const RecordPlace& place,
                                     std::uint32_t inode, std::uint32_t replacement,
                                     std::uint8_t file_type, std::span<std::uint8_t> block)
{
    if (const Status read = ReadMetadata(place.physical, 0, block, block); !read)
    {
        return read;
    }
    const Result<RawEntry> record =
        ParseEntry(block, place.offset, has_file_type, superblock.inodes_count);
    if (!record)
    {
        return Failure(record);
    }
    if (record->inode != inode)
    {
        return std::unexpected(Error::Corrupt);
    }
    StoreLittleEndian(block.data() + place.offset, replacement);
    if (has_file_type)
    {
        block[place.offset + 7] = file_type;
    }
    return WriteDirectoryBlock(directory, place.logical, place.physical, block);
}

std::uint16_t Ext2Filesystem::LinkLimit() const
{
    return directory_links ? MaximumExt4Links : MaximumLinks;
}

std::optional<std::uint16_t> Ext2Filesystem::DirectoryLinks(std::uint16_t count, int delta) const
{
    if (directory_links && count == 1)
    {
        return count;
    }
    // A directory always has "." and its parent's entry.
    const int links = int{count} + delta;
    if (links < 2)
    {
        return std::nullopt;
    }
    if (links > LinkLimit())
    {
        return directory_links ? std::optional<std::uint16_t>(1) : std::nullopt;
    }
    return static_cast<std::uint16_t>(links);
}

bool Ext2Filesystem::HasSubdirectoryLink(std::uint16_t count) const
{
    return count >= 3 || (directory_links && count == 1);
}

Status Ext2Filesystem::UpdateDirectory(NodeId directory, int links, bool touched,
                                       std::span<std::uint8_t> scratch)
{
    Result<Inode> inode = LoadInode(directory, scratch);
    if (!inode)
    {
        return Failure(inode);
    }
    const std::optional<std::uint16_t> count = DirectoryLinks(inode->link_count, links);
    if (!count)
    {
        return std::unexpected(Error::Corrupt);
    }
    inode->link_count = *count;
    if (touched)
    {
        const Timestamp now = Now();
        inode->modify_time = now;
        inode->change_time = now;
    }
    return StoreInode(directory, *inode, false, scratch);
}

Result<bool> Ext2Filesystem::IsEmptyDirectory(const Inode& directory, NodeId parent,
                                              Buffers& buffers)
{
    bool empty = true;
    bool parent_found = false;
    const Result<bool> walked =
        WalkDirectory(directory, 0, buffers[DataBuffer], buffers[ScratchBuffer],
                      [&](const RawEntry& entry, const RecordPlace&) -> Result<bool>
                      {
                          if (entry.inode == 0 || entry.name == ".")
                          {
                              return false;
                          }
                          if (entry.name == "..")
                          {
                              // The parent's link count holds this link, which goes with it.
                              if (entry.inode != parent)
                              {
                                  return std::unexpected(Error::Corrupt);
                              }
                              parent_found = true;
                              return false;
                          }
                          empty = false;
                          return true;
                      });
    if (!walked)
    {
        return Failure(walked);
    }
    if (empty && !parent_found)
    {
        return std::unexpected(Error::Corrupt);
    }
    return empty;
}

// A directory must not move into itself or below it: from its new parent up to the root, no
// directory may be the one moving. A volume has fewer directories than inodes, so that many steps
// up ".." reach the root unless the ".." entries loop.
Status Ext2Filesystem::CheckOutside(NodeId directory, NodeId moving, Buffers& buffers)
{
    NodeId current = directory;
    for (std::uint32_t step = 0; step <= superblock.inodes_count; ++step)
    {
        if (current == moving)
        {
            return std::unexpected(Error::InvalidArgument);
        }
        if (current == RootInode)
        {
            return {};
        }
        const Result<Node> node = LoadNode(current, buffers[ScratchBuffer]);
        if (!node)
        {
            return std::unexpected(Dangling(node.error()));
        }
        if (node->type != NodeType::Directory)
        {
            return std::unexpected(Error::Corrupt);
        }
        const Result<NameSearch> parent = SearchDirectory(node->inode, "..", 0, buffers);
        if (!parent)
        {
            return Failure(parent);
        }
        if (!parent->place)
        {
            return std::unexpected(Error::Corrupt);
        }
        current = parent->inode;
    }
    return std::unexpected(Error::Corrupt);
}

// With no links and a deletion time, e2fsck counts an inode free whatever it still points to, so
// it is written so before its blocks and itself go back to the bitmaps. A directory's ".." stops
// linking its parent at that point too, and only then leaves the parent's count.
Status Ext2Filesystem::DropLink(NodeId node, const Node& loaded, NodeId parent, Buffers& buffers)
{
    Inode inode = loaded.inode;
    const Timestamp now = Now();
    inode.change_time = now;
    const bool directory = loaded.type == NodeType::Directory;
    if (!directory && inode.link_count > 1)
    {
        --inode.link_count;
        return StoreInode(node, inode, false, buffers[ScratchBuffer]);
    }
    // An empty directory's two links, its entry and its own ".", go together.
    inode.link_count = 0;
    inode.deletion_time = static_cast<std::uint32_t>(now.seconds);
    if (const Status stored = StoreInode(node, inode, false, buffers[ScratchBuffer]); !stored)
    {
        return stored;
    }
    if (directory)
    {
        if (const Status counted = UpdateDirectory(parent, -1, true, buffers[ScratchBuffer]);
            !counted)
        {
            return counted;
        }
    }
    if (const Status released = ReleaseContents({.inode = inode, .type = loaded.type}, buffers);
        !released)
    {
        return released;
    }
    return FreeInode(node, directory, buffers[BitmapBuffer]);
}

Result<NodeId> Ext2Filesystem::CreateNode(NodeId directory, std::string_view name,
                                          std::uint16_t mode, std::string_view target)
{
    if (const Status valid = CheckName(name); !valid)
    {
        return Failure(valid);
    }
    if (IsDots(name))
    {
        return std::unexpected(Error::Exists);
    }
    const bool is_directory = (mode & ModeTypeMask) == ModeDirectory;
    const bool is_link = (mode & ModeTypeMask) == ModeSymbolicLink;
    // A target shorter than i_block is kept there ("fast"); a longer one gets a block ("slow").
    const bool needs_block = is_directory || (is_link && target.size() >= BlockArrayBytes);
    Buffers buffers(block_size, ChangeBuffers);
    if (buffers.Empty())
    {
        return std::unexpected(Error::OutOfMemory);
    }
    const std::span<std::uint8_t> scratch = buffers[ScratchBuffer];
    const std::span<std::uint8_t> bitmap = buffers[BitmapBuffer];
    const Result<Node> parent = LoadNode(directory, scratch);
    if (!parent)
    {
        return Failure(parent);
    }
    if (parent->type != NodeType::Directory)
    {
        return std::unexpected(Error::NotDirectory);
    }
    if (is_directory && !directory_links && parent->inode.link_count >= MaximumLinks)
    {
        return std::unexpected(Error::TooManyLinks);
    }
    const Result<NameSearch> search =
        SearchDirectory(parent->inode, name, RecordLength(name.size()), buffers);
    if (!search)
    {
        return Failure(search);
    }
    if (search->place)
    {
        return std::unexpected(Error::Exists);
    }
    if (const Status begun = BeginChange(scratch); !begun)
    {
        return Failure(begun);
    }
    Inode directory_inode = parent->inode;
    if (const Status cleared = ClearIndex(directory, directory_inode, buffers); !cleared)
    {
        return Failure(cleared);
    }
    const std::uint32_t parent_group = GroupOfInode(directory);
    const Result<NodeId> node = AllocateInode(
        is_directory ? DirectoryGroup(parent_group, directory == RootInode) : parent_group,
        is_directory, bitmap, scratch);
    if (!node)
    {
        return node;
    }

    const Timestamp now = Now();
    Inode inode = NewInode(mode, now);
    // As on Linux: files, directories and links with a block of their own; a fast link's target
    // fills i_block instead.
    const bool extent_mapped =
        extents && ((mode & ModeTypeMask) == ModeRegular || is_directory || needs_block);
    if (extent_mapped)
    {
        inode.flags |= InodeExtents;
        StoreExtentHeader(inode.blocks, {.entries = 0, .maximum = RootExtentEntries, .depth = 0});
    }
    std::uint32_t block = 0;
    Status status;
    if (needs_block)
    {
        const Result<std::uint32_t> allocated =
            AllocateBlock(static_cast<std::uint32_t>(GroupFirstBlock(GroupOfInode(*node))), bitmap);
        if (allocated)
        {
            block = *allocated;
            const std::span<std::uint8_t> contents = buffers[DataBuffer];
            ZeroBytes(contents);
            if (is_directory)
            {
                const std::uint8_t type =
                    has_file_type ? EntryFileType(ModeDirectory) : std::uint8_t{0};
                StoreEntry(contents, 0,
                           {.inode = static_cast<std::uint32_t>(*node),
                            .record_length = DotRecordLength,
                            .file_type = type,
                            .name = "."},
                           has_file_type);
                const std::size_t records_end =
                    metadata_checksums ? block_size - DirectoryTailSize : block_size;
                StoreEntry(
                    contents, DotRecordLength,
                    {.inode = static_cast<std::uint32_t>(directory),
                     .record_length = static_cast<std::uint16_t>(records_end - DotRecordLength),
                     .file_type = type,
                     .name = ".."},
                    has_file_type);
                if (metadata_checksums)
                {
                    StoreDirectoryTail(
                        contents, InodeChecksumSeed(volume_seed, static_cast<std::uint32_t>(*node),
                                                    inode.generation));
                }
                inode.size = block_size;
                inode.link_count = 2;
            }
            else
            {
                CopyBytes(contents.data(), target.data(), target.size());
                inode.size = target.size();
            }
            if (extent_mapped)
            {
                StoreExtentHeader(inode.blocks,
                                  {.entries = 1, .maximum = RootExtentEntries, .depth = 0});
                StoreExtent(inode.blocks, 0,
                            {.logical = 0, .length = 1, .physical = block, .unwritten = false});
            }
            else
            {
                inode.SetBlock(0, block);
            }
            inode.allocated_bytes = block_size;
            status = WriteMetadata(block, contents);
        }
        else
        {
            status = Failure(allocated);
        }
    }
    else if (is_link)
    {
        CopyBytes(inode.blocks.data(), target.data(), target.size());
        inode.size = target.size();
    }
    // The new directory's ".." links the parent once the inode is in use.
    bool parent_counted = false;
    const std::uint16_t parent_links = directory_inode.link_count;
    if (status && is_directory)
    {
        const std::optional<std::uint16_t> count = DirectoryLinks(parent_links, 1);
        if (count)
        {
            directory_inode.link_count = *count;
            status = StoreInode(directory, directory_inode, false, scratch);
            parent_counted = status.has_value();
        }
        else
        {
            status = std::unexpected(Error::Corrupt);
        }
    }
    if (status)
    {
        status = StoreInode(*node, inode, true, scratch);
    }
    if (status)
    {
        const RawEntry entry = {
            .inode = static_cast<std::uint32_t>(*node),
            .record_length = 0,
            .file_type = has_file_type ? EntryFileType(inode.mode) : std::uint8_t{0},
            .name = name,
        };
        status = AddEntry(directory, directory_inode, search->room, entry, buffers);
    }
    if (status)
    {
        return node;
    }
    // Nothing names the inode, so it goes back, deleted before the parent's count and the
    // bitmaps. If a step of this fails too, the inode and its block only leak.
    inode.link_count = 0;
    inode.deletion_time = static_cast<std::uint32_t>(now.seconds);
    if (!StoreInode(*node, inode, true, scratch))
    {
        return Failure(status);
    }
    if (parent_counted)
    {
        directory_inode.link_count = parent_links;
        [[maybe_unused]] const Status restored =
            StoreInode(directory, directory_inode, false, scratch);
    }
    FreeCursor frees = {.bitmap = bitmap, .group = 0, .loaded = false, .dirty = false};
    if (block == 0 || (FreeBlock(frees, block) && FinishFrees(frees)))
    {
        [[maybe_unused]] const Status freed = FreeInode(*node, is_directory, bitmap);
    }
    return Failure(status);
}

Status Ext2Filesystem::RemoveNode(NodeId directory, std::string_view name, bool directory_wanted)
{
    if (IsDots(name))
    {
        return std::unexpected(directory_wanted ? Error::InvalidArgument : Error::IsDirectory);
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
    const Result<NameSearch> search = SearchDirectory(parent->inode, name, 0, buffers);
    if (!search)
    {
        return Failure(search);
    }
    if (!search->place)
    {
        return std::unexpected(Error::NotFound);
    }
    const std::uint32_t target = search->inode;
    if (target == RootInode)
    {
        return std::unexpected(Error::Busy);
    }
    if (IsReserved(target))
    {
        return std::unexpected(Error::InvalidArgument);
    }
    const Result<Node> node = LoadNode(target, scratch);
    if (!node)
    {
        return std::unexpected(Dangling(node.error()));
    }
    if (directory_wanted)
    {
        if (node->type != NodeType::Directory)
        {
            return std::unexpected(Error::NotDirectory);
        }
        const Result<bool> empty = IsEmptyDirectory(node->inode, directory, buffers);
        if (!empty)
        {
            return Failure(empty);
        }
        if (!*empty)
        {
            return std::unexpected(Error::NotEmpty);
        }
        if (!HasSubdirectoryLink(parent->inode.link_count))
        {
            return std::unexpected(Error::Corrupt);
        }
    }
    else if (node->type == NodeType::Directory)
    {
        return std::unexpected(Error::IsDirectory);
    }
    if (directory_wanted || node->inode.link_count <= 1)
    {
        if (const Status releasable = CheckReleasable(node->inode, buffers[BitmapBuffer]);
            !releasable)
        {
            return releasable;
        }
    }
    if (const Status begun = BeginChange(scratch); !begun)
    {
        return begun;
    }
    Inode directory_inode = parent->inode;
    if (const Status dropped =
            DropRecord(directory_inode, *search->place, target, buffers[DataBuffer]);
        !dropped)
    {
        return dropped;
    }
    if (!directory_wanted)
    {
        const Timestamp now = Now();
        directory_inode.modify_time = now;
        directory_inode.change_time = now;
        if (const Status stored = StoreInode(directory, directory_inode, false, scratch); !stored)
        {
            return stored;
        }
    }
    return DropLink(target, *node, directory, buffers);
}

Status Ext2Filesystem::LinkNode(NodeId node, NodeId directory, std::string_view name)
{
    if (const Status valid = CheckName(name); !valid)
    {
        return valid;
    }
    if (IsDots(name))
    {
        return std::unexpected(Error::Exists);
    }
    if (IsReserved(node))
    {
        return std::unexpected(Error::InvalidArgument);
    }
    Buffers buffers(block_size, ChangeBuffers);
    if (buffers.Empty())
    {
        return std::unexpected(Error::OutOfMemory);
    }
    const std::span<std::uint8_t> scratch = buffers[ScratchBuffer];
    const Result<Node> target = LoadNode(node, scratch);
    if (!target)
    {
        return Failure(target);
    }
    // As on Linux: a directory's second name would let the tree loop.
    if (target->type == NodeType::Directory)
    {
        return std::unexpected(Error::NotPermitted);
    }
    if (target->inode.link_count >= LinkLimit())
    {
        return std::unexpected(Error::TooManyLinks);
    }
    const Result<Node> parent = LoadNode(directory, scratch);
    if (!parent)
    {
        return Failure(parent);
    }
    if (parent->type != NodeType::Directory)
    {
        return std::unexpected(Error::NotDirectory);
    }
    const Result<NameSearch> search =
        SearchDirectory(parent->inode, name, RecordLength(name.size()), buffers);
    if (!search)
    {
        return Failure(search);
    }
    if (search->place)
    {
        return std::unexpected(Error::Exists);
    }
    if (const Status begun = BeginChange(scratch); !begun)
    {
        return begun;
    }
    Inode directory_inode = parent->inode;
    if (const Status cleared = ClearIndex(directory, directory_inode, buffers); !cleared)
    {
        return cleared;
    }
    Inode inode = target->inode;
    ++inode.link_count;
    inode.change_time = Now();
    if (const Status stored = StoreInode(node, inode, false, scratch); !stored)
    {
        return stored;
    }
    const RawEntry entry = {
        .inode = static_cast<std::uint32_t>(node),
        .record_length = 0,
        .file_type = has_file_type ? EntryFileType(inode.mode) : std::uint8_t{0},
        .name = name,
    };
    if (const Status added = AddEntry(directory, directory_inode, search->room, entry, buffers);
        !added)
    {
        --inode.link_count;
        [[maybe_unused]] const Status restored = StoreInode(node, inode, false, scratch);
        return added;
    }
    return {};
}

Status Ext2Filesystem::RenameNode(NodeId from_directory, std::string_view from_name,
                                  NodeId to_directory, std::string_view to_name)
{
    if (IsDots(from_name) || IsDots(to_name))
    {
        return std::unexpected(Error::InvalidArgument);
    }
    if (const Status valid = CheckName(from_name); !valid)
    {
        return valid;
    }
    if (const Status valid = CheckName(to_name); !valid)
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
    const Result<Node> from_parent = LoadNode(from_directory, scratch);
    if (!from_parent)
    {
        return Failure(from_parent);
    }
    const Result<Node> to_parent = LoadNode(to_directory, scratch);
    if (!to_parent)
    {
        return Failure(to_parent);
    }
    if (from_parent->type != NodeType::Directory || to_parent->type != NodeType::Directory)
    {
        return std::unexpected(Error::NotDirectory);
    }
    const Result<NameSearch> source = SearchDirectory(from_parent->inode, from_name, 0, buffers);
    if (!source)
    {
        return Failure(source);
    }
    if (!source->place)
    {
        return std::unexpected(Error::NotFound);
    }
    const std::uint32_t moving = source->inode;
    if (moving == RootInode)
    {
        return std::unexpected(Error::Busy);
    }
    if (IsReserved(moving))
    {
        return std::unexpected(Error::InvalidArgument);
    }
    const Result<Node> moved = LoadNode(moving, scratch);
    if (!moved)
    {
        return std::unexpected(Dangling(moved.error()));
    }
    const Result<NameSearch> destination =
        SearchDirectory(to_parent->inode, to_name, RecordLength(to_name.size()), buffers);
    if (!destination)
    {
        return Failure(destination);
    }
    // Two names of one inode, or a name onto itself: POSIX says rename does nothing.
    if (destination->place && destination->inode == moving)
    {
        return {};
    }
    const bool is_directory = moved->type == NodeType::Directory;
    const bool new_parent = from_directory != to_directory;
    const bool reparented = is_directory && new_parent;
    // The record of what to_name names now, which the rename replaces.
    std::optional<RecordPlace> replaced;
    const std::uint32_t replaced_inode = destination->place ? destination->inode : 0;
    if (destination->place)
    {
        if (replaced_inode == RootInode)
        {
            return std::unexpected(Error::Busy);
        }
        if (IsReserved(replaced_inode))
        {
            return std::unexpected(Error::InvalidArgument);
        }
        const Result<Node> existing = LoadNode(replaced_inode, scratch);
        if (!existing)
        {
            return std::unexpected(Dangling(existing.error()));
        }
        const bool replaced_directory = existing->type == NodeType::Directory;
        if (is_directory != replaced_directory)
        {
            return std::unexpected(is_directory ? Error::NotDirectory : Error::IsDirectory);
        }
        if (replaced_directory)
        {
            const Result<bool> empty = IsEmptyDirectory(existing->inode, to_directory, buffers);
            if (!empty)
            {
                return Failure(empty);
            }
            if (!*empty)
            {
                return std::unexpected(Error::NotEmpty);
            }
            if (!HasSubdirectoryLink(to_parent->inode.link_count))
            {
                return std::unexpected(Error::Corrupt);
            }
        }
        if (replaced_directory || existing->inode.link_count <= 1)
        {
            if (const Status releasable = CheckReleasable(existing->inode, buffers[BitmapBuffer]);
                !releasable)
            {
                return releasable;
            }
        }
        replaced = destination->place;
    }
    std::optional<RecordPlace> dot_dot;
    if (reparented)
    {
        if (const Status outside = CheckOutside(to_directory, moving, buffers); !outside)
        {
            return outside;
        }
        if (!replaced && !directory_links && to_parent->inode.link_count >= MaximumLinks)
        {
            return std::unexpected(Error::TooManyLinks);
        }
        const Result<NameSearch> parent_entry = SearchDirectory(moved->inode, "..", 0, buffers);
        if (!parent_entry)
        {
            return Failure(parent_entry);
        }
        if (!parent_entry->place || parent_entry->inode != from_directory ||
            !HasSubdirectoryLink(from_parent->inode.link_count))
        {
            return std::unexpected(Error::Corrupt);
        }
        dot_dot = parent_entry->place;
    }

    if (const Status begun = BeginChange(scratch); !begun)
    {
        return begun;
    }
    // Only a new record changes what a hash index would have to say.
    if (!replaced)
    {
        Inode to_inode = to_parent->inode;
        if (const Status cleared = ClearIndex(to_directory, to_inode, buffers); !cleared)
        {
            return cleared;
        }
    }
    // 1: the moving directory's ".." will count for its new parent.
    if (reparented)
    {
        if (const Status counted = UpdateDirectory(to_directory, 1, false, scratch); !counted)
        {
            return counted;
        }
    }
    // 2: the new name.
    const std::uint8_t file_type =
        has_file_type ? EntryFileType(moved->inode.mode) : std::uint8_t{0};
    if (replaced)
    {
        if (const Status pointed = RewriteRecord(to_parent->inode, *replaced, replaced_inode,
                                                 moving, file_type, block);
            !pointed)
        {
            return pointed;
        }
        if (const Status touched = UpdateDirectory(to_directory, 0, true, scratch); !touched)
        {
            return touched;
        }
    }
    else
    {
        Result<Inode> to_inode = LoadInode(to_directory, scratch);
        if (!to_inode)
        {
            return Failure(to_inode);
        }
        const RawEntry entry = {
            .inode = moving, .record_length = 0, .file_type = file_type, .name = to_name};
        if (const Status added =
                AddEntry(to_directory, *to_inode, destination->room, entry, buffers);
            !added)
        {
            if (reparented)
            {
                [[maybe_unused]] const Status restored =
                    UpdateDirectory(to_directory, -1, false, scratch);
            }
            return added;
        }
    }
    // 3: the old name. Adding may have moved records of the same directory, so it is found again.
    {
        const Result<Inode> current = LoadInode(from_directory, scratch);
        if (!current)
        {
            return Failure(current);
        }
        const Result<NameSearch> again = SearchDirectory(*current, from_name, 0, buffers);
        if (!again)
        {
            return Failure(again);
        }
        if (!again->place || again->inode != moving)
        {
            return std::unexpected(Error::Corrupt);
        }
        if (const Status dropped = DropRecord(*current, *again->place, moving, block); !dropped)
        {
            return dropped;
        }
        if (const Status touched = UpdateDirectory(from_directory, 0, true, scratch); !touched)
        {
            return touched;
        }
    }
    // 4: a moved directory's "..", then its old parent's count.
    if (dot_dot)
    {
        if (const Status pointed = RewriteRecord(
                moved->inode, *dot_dot, static_cast<std::uint32_t>(from_directory),
                static_cast<std::uint32_t>(to_directory),
                has_file_type ? EntryFileType(ModeDirectory) : std::uint8_t{0}, block);
            !pointed)
        {
            return pointed;
        }
        if (const Status counted = UpdateDirectory(from_directory, -1, false, scratch); !counted)
        {
            return counted;
        }
    }
    // 5: the renamed inode's change time.
    {
        Result<Inode> inode = LoadInode(moving, scratch);
        if (!inode)
        {
            return Failure(inode);
        }
        inode->change_time = Now();
        if (const Status stored = StoreInode(moving, *inode, false, scratch); !stored)
        {
            return stored;
        }
    }
    // 6: what the new name replaced.
    if (!replaced)
    {
        return {};
    }
    const Result<Node> existing = LoadNode(replaced_inode, scratch);
    if (!existing)
    {
        return Failure(existing);
    }
    return DropLink(replaced_inode, *existing, to_directory, buffers);
}

Result<NodeId> Ext2Filesystem::Create(NodeId directory, std::string_view name, std::uint32_t mode)
{
    if (read_only)
    {
        return Refused();
    }
    return Noted(CreateNode(
        directory, name, static_cast<std::uint16_t>(ModeRegular | (mode & ModePermissions)), {}));
}

Result<NodeId> Ext2Filesystem::MakeDirectory(NodeId directory, std::string_view name,
                                             std::uint32_t mode)
{
    if (read_only)
    {
        return Refused();
    }
    return Noted(CreateNode(
        directory, name, static_cast<std::uint16_t>(ModeDirectory | (mode & ModePermissions)), {}));
}

Result<NodeId> Ext2Filesystem::SymbolicLink(NodeId directory, std::string_view name,
                                            std::string_view target)
{
    if (read_only)
    {
        return Refused();
    }
    if (target.size() >= MaximumPathLength)
    {
        return std::unexpected(Error::NameTooLong);
    }
    if (target.empty() || target.find('\0') != std::string_view::npos)
    {
        return std::unexpected(Error::InvalidArgument);
    }
    return Noted(CreateNode(directory, name, ModeSymbolicLink | LinkPermissions, target));
}

Status Ext2Filesystem::Remove(NodeId directory, std::string_view name)
{
    if (read_only)
    {
        return Refused();
    }
    return Noted(RemoveNode(directory, name, false));
}

Status Ext2Filesystem::RemoveDirectory(NodeId directory, std::string_view name)
{
    if (read_only)
    {
        return Refused();
    }
    return Noted(RemoveNode(directory, name, true));
}

Status Ext2Filesystem::Rename(NodeId from_directory, std::string_view from_name,
                              NodeId to_directory, std::string_view to_name)
{
    if (read_only)
    {
        return Refused();
    }
    return Noted(RenameNode(from_directory, from_name, to_directory, to_name));
}

Status Ext2Filesystem::Link(NodeId node, NodeId directory, std::string_view name)
{
    if (read_only)
    {
        return Refused();
    }
    return Noted(LinkNode(node, directory, name));
}

} // namespace FeatherDisk::Ext
