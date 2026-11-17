// SPDX-License-Identifier: MIT OR Apache-2.0
#include "fs/ext2.h"

#include "common/bytes.h"
#include "common/little_endian.h"
#include "ext/ext2_filesystem.h"
#include "ext/features.h"
#include "fs/host.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace FeatherDisk::Ext
{
namespace
{

// Metadata blocks each mounted volume keeps in memory.
constexpr std::size_t CacheBytes = 32 * 1024;
constexpr std::uint32_t PointerSize = 4;
// i_blocks counts 512-byte sectors in 32 bits without huge_file, which caps a file at 2 TiB;
// without large_file, sizes stay below 2 GiB.
constexpr std::uint64_t MaximumAllocatedBytes = std::uint64_t{0xFFFFFFFF} * 512;
constexpr std::uint64_t MaximumSmallFileBytes = 0x7FFFFFFF;

// Appends text to a log line, leaving room for what LogWithValue adds.
void Append(std::array<char, 128>& line, std::size_t& length, std::string_view text)
{
    for (const char character : text)
    {
        if (length < line.size() - 10)
        {
            line[length++] = character;
        }
    }
}

Result<Superblock> ReadSuperblock(BlockDevice& volume)
{
    const std::uint32_t sector_size = volume.SectorSize();
    if (sector_size == 0 || !std::has_single_bit(sector_size))
    {
        return std::unexpected(Error::Unsupported);
    }
    const std::uint64_t first_sector = SuperblockOffset / sector_size;
    const std::uint64_t end_sector =
        DivideRoundingUp(SuperblockOffset + SuperblockSize, sector_size);
    if (volume.SectorCount() < end_sector)
    {
        return std::unexpected(Error::Unsupported);
    }
    HostBuffer buffer(static_cast<std::size_t>((end_sector - first_sector) * sector_size));
    if (buffer.Empty())
    {
        return std::unexpected(Error::OutOfMemory);
    }
    if (const Status read = volume.Read(first_sector, buffer.Bytes()); !read)
    {
        return Failure(read);
    }
    const std::span<const std::uint8_t> bytes = buffer.Bytes().subspan(
        static_cast<std::size_t>(SuperblockOffset - first_sector * sector_size));
    return DecodeSuperblock(bytes.first<SuperblockSize>());
}

// Why a volume this driver reads must not be written, or nullopt. Only plain ext2 is written:
// a journal or any ext4 feature keeps the volume read-only.
std::optional<std::string_view> WriteRefusal(const Superblock& superblock)
{
    if ((superblock.compatible & CompatibleHasJournal) != 0)
    {
        return "a journal, which writes would have to go through";
    }
    if ((superblock.incompatible & ~IncompatibleWritable) != 0 ||
        (superblock.read_only_compatible &
         (ReadOnlyCompatibleHugeFile | ReadOnlyCompatibleGroupChecksum |
          ReadOnlyCompatibleDirectoryLinks | ReadOnlyCompatibleExtraInodeSize |
          ReadOnlyCompatibleMetadataChecksum)) != 0)
    {
        return "ext4 features, which only the read path supports";
    }
    if ((superblock.read_only_compatible & ~ReadOnlyCompatibleWritable) != 0)
    {
        return "unknown read-only compatible features";
    }
    if ((superblock.compatible & ~CompatibleWritable) != 0)
    {
        return "compatible features whose rules it does not keep";
    }
    // New inodes are taken from s_first_ino on; the ones below are reserved.
    if (superblock.first_inode < GoodOldFirstInode ||
        superblock.first_inode > superblock.inodes_count)
    {
        return "the first ordinary inode number is impossible";
    }
    if ((superblock.state & StateErrors) != 0)
    {
        return "errors are recorded; run e2fsck";
    }
    if ((superblock.state & StateClean) == 0)
    {
        return "it was not cleanly unmounted; run e2fsck";
    }
    return std::nullopt;
}

class Ext2FilesystemType final : public FilesystemType
{
public:
    constexpr Ext2FilesystemType() = default;

    std::string_view Name() const override;
    bool Probe(BlockDevice& volume) override;
    Result<Filesystem*> Mount(BlockDevice& volume, MountOptions options) override;
    Status Unmount(Filesystem& filesystem) override;
};

} // namespace

std::uint64_t DivideRoundingUp(std::uint64_t value, std::uint64_t divisor)
{
    return value / divisor + (value % divisor != 0 ? 1 : 0);
}

void LogWarning(std::string_view message, std::string_view detail)
{
    std::array<char, 128> line{};
    std::size_t length = 0;
    Append(line, length, message);
    Append(line, length, detail);
    Host::Log(Host::LogLevel::Warning, std::string_view(line.data(), length));
}

void LogWithValue(std::string_view message, std::uint32_t value)
{
    constexpr std::string_view Digits = "0123456789abcdef";
    std::array<char, 128> line{};
    std::size_t length = 0;
    Append(line, length, message);
    line[length++] = '0';
    line[length++] = 'x';
    for (int shift = 28; shift >= 0; shift -= 4)
    {
        line[length++] = Digits[(value >> shift) & 0xFU];
    }
    Host::Log(Host::LogLevel::Warning, std::string_view(line.data(), length));
}

Buffers::Buffers(std::uint32_t block_size, std::size_t count)
    : memory(std::size_t{block_size} * count), size(block_size)
{
}

bool Buffers::Empty() const
{
    return memory.Empty();
}

std::span<std::uint8_t> Buffers::operator[](std::size_t index)
{
    return memory.Bytes().subspan(index * size, size);
}

Ext2Filesystem::Ext2Filesystem(BlockDevice& device, const Superblock& layout,
                               bool mounted_read_only)
    : volume(device), superblock(layout),
      descriptor_memory(static_cast<std::size_t>(
          DivideRoundingUp(std::uint64_t{layout.group_count} * layout.descriptor_size,
                           layout.block_size) *
          layout.block_size)),
      writable_mount(false), read_only(mounted_read_only),
      has_file_type((layout.incompatible & IncompatibleFileType) != 0),
      group_checksums(HasGroupChecksums(layout)), metadata_checksums(HasMetadataChecksums(layout)),
      hashed_directories((layout.compatible & CompatibleDirectoryIndex) != 0),
      volume_seed(VolumeChecksumSeed(layout)), descriptor_size(layout.descriptor_size),
      block_size(layout.block_size), pointers_per_block(layout.block_size / PointerSize),
      inode_table_blocks(DivideRoundingUp(
          std::uint64_t{layout.inodes_per_group} * layout.inode_size, layout.block_size)),
      descriptor_blocks(DivideRoundingUp(std::uint64_t{layout.group_count} * layout.descriptor_size,
                                         layout.block_size))
{
}

void* Ext2Filesystem::operator new(std::size_t /*size*/, void* place) noexcept
{
    return place;
}

Status Ext2Filesystem::Initialize()
{
    if (const Status initialized = cache.Initialize(volume, block_size, CacheBytes / block_size);
        !initialized)
    {
        return initialized;
    }
    if (descriptor_memory.Empty())
    {
        return std::unexpected(Error::OutOfMemory);
    }
    // The group descriptor table follows the superblock's block, and CheckSuperblock made sure
    // it fits. It stays in memory: its free counts change with every allocation.
    descriptors = descriptor_memory.Bytes();
    if (const Status read = cache.ReadUncached(superblock.first_data_block + 1, descriptors); !read)
    {
        return read;
    }
    if (const Status checked = CheckDescriptorChecksums(); !checked)
    {
        return checked;
    }
    Buffers buffers(block_size, 1);
    if (buffers.Empty())
    {
        return std::unexpected(Error::OutOfMemory);
    }
    const Result<Node> root = LoadNode(RootInode, buffers[0]);
    if (!root)
    {
        return std::unexpected(root.error() == Error::NotFound ? Error::Corrupt : root.error());
    }
    if (root->type != NodeType::Directory)
    {
        return std::unexpected(Error::Corrupt);
    }
    if (read_only)
    {
        return {};
    }
    if (const std::optional<std::string_view> reason = CheckGroups())
    {
        LogWarning("ext2: mounted read-only: ", *reason);
        read_only = true;
        return {};
    }
    return MarkMounted();
}

Status Ext2Filesystem::CheckDescriptorChecksums() const
{
    if (!group_checksums)
    {
        return {};
    }
    for (std::uint32_t group = 0; group < superblock.group_count; ++group)
    {
        const std::span<const std::uint8_t> bytes =
            descriptors.subspan(std::size_t{group} * descriptor_size, descriptor_size);
        if (GroupDescriptorChecksum(superblock, volume_seed, group, bytes) != Group(group).checksum)
        {
            LogWithValue("ext2: group descriptor checksum mismatch in group ", group);
            return std::unexpected(Error::Corrupt);
        }
    }
    return {};
}

std::unexpected<Error> Ext2Filesystem::Refused() const
{
    return std::unexpected(read_only ? Error::ReadOnly : Error::Unsupported);
}

// Block 0 (and block 1 with 1 KiB blocks) holds the superblock, so no pointer may name it; 0 in
// a block map is a hole instead.
bool Ext2Filesystem::IsDataBlock(std::uint64_t block) const
{
    return block > superblock.first_data_block && block < superblock.blocks_count;
}

std::uint64_t Ext2Filesystem::MaximumFileSize() const
{
    const std::uint64_t per_block = pointers_per_block;
    const std::uint64_t mapped_blocks =
        DirectBlocks + per_block + per_block * per_block + per_block * per_block * per_block;
    std::uint64_t maximum = mapped_blocks * block_size;
    if (maximum > MaximumAllocatedBytes)
    {
        maximum = MaximumAllocatedBytes;
    }
    // A writable revision 1 volume gains large_file with its first large file, as on Linux; a
    // revision 0 volume is not upgraded to make room for the feature.
    const bool large_files = (superblock.read_only_compatible & ReadOnlyCompatibleLargeFile) != 0 ||
                             (!read_only && superblock.revision >= DynamicRevision);
    if (!large_files && maximum > MaximumSmallFileBytes)
    {
        maximum = MaximumSmallFileBytes;
    }
    return maximum;
}

Result<Ext2Filesystem::InodePlace> Ext2Filesystem::LocateInode(NodeId node) const
{
    if (node == 0 || node > superblock.inodes_count)
    {
        return std::unexpected(Error::InvalidArgument);
    }
    // Inodes count from 1, in groups of inodes_per_group, each group with its own inode table
    // that its descriptor locates.
    const auto number = static_cast<std::uint32_t>(node - 1);
    const std::uint64_t table = Group(number / superblock.inodes_per_group).inode_table;
    if (!IsDataBlock(table) || table + inode_table_blocks > superblock.blocks_count)
    {
        return std::unexpected(Error::Corrupt);
    }
    const std::uint64_t byte =
        std::uint64_t{number % superblock.inodes_per_group} * superblock.inode_size;
    return InodePlace{
        .block = table + byte / block_size,
        .offset = static_cast<std::size_t>(byte % block_size),
    };
}

// A group whose inodes were never used may have no inode table written (lazy_itable_init), so
// what is there must not be read as inodes.
bool Ext2Filesystem::IsUnusedInode(NodeId node) const
{
    if (!group_checksums)
    {
        return false;
    }
    const auto number = static_cast<std::uint32_t>(node - 1);
    const GroupDescriptor descriptor = Group(number / superblock.inodes_per_group);
    if ((descriptor.flags & GroupInodesUnused) != 0)
    {
        return true;
    }
    const std::uint32_t unused = descriptor.unused_inodes < superblock.inodes_per_group
                                     ? descriptor.unused_inodes
                                     : superblock.inodes_per_group;
    return number % superblock.inodes_per_group >= superblock.inodes_per_group - unused;
}

Result<Inode> Ext2Filesystem::LoadInode(NodeId node, std::span<std::uint8_t> scratch)
{
    const Result<InodePlace> place = LocateInode(node);
    if (!place)
    {
        return Failure(place);
    }
    if (IsUnusedInode(node))
    {
        return Inode{};
    }
    const std::size_t length =
        superblock.inode_size < DecodedInodeBytes ? superblock.inode_size : DecodedInodeBytes;
    std::array<std::uint8_t, DecodedInodeBytes> copy{};
    std::span<const std::uint8_t> bytes(copy.data(), length);
    std::uint32_t inode_seed = 0;
    if (metadata_checksums)
    {
        // The checksum covers the whole inode, so its block is read whole.
        if (const Status read = cache.Read(place->block, 0, scratch, scratch); !read)
        {
            return Failure(read);
        }
        const std::span<const std::uint8_t> whole =
            scratch.subspan(place->offset, superblock.inode_size);
        bool zero = true;
        for (const std::uint8_t byte : whole)
        {
            zero = zero && byte == 0;
        }
        // An inode that was never written has no checksum either, as e2fsck allows.
        if (zero)
        {
            return Inode{};
        }
        inode_seed = InodeChecksumSeed(volume_seed, static_cast<std::uint32_t>(node),
                                       LoadLittleEndian<std::uint32_t>(whole.data() + 100));
        if (!InodeChecksumMatches(whole, inode_seed))
        {
            LogWithValue("ext2: inode checksum mismatch in inode ",
                         static_cast<std::uint32_t>(node));
            return std::unexpected(Error::Corrupt);
        }
        bytes = whole.first(length);
    }
    else if (const Status read = cache.Read(place->block, place->offset,
                                            std::span<std::uint8_t>(copy.data(), length), scratch);
             !read)
    {
        return Failure(read);
    }
    Inode inode = DecodeInode(bytes, superblock.read_only_compatible, block_size);
    // The extra fields must fit in the inode and keep 4-byte alignment.
    if (GoodOldInodeSize + std::uint32_t{inode.extra_size} > superblock.inode_size ||
        inode.extra_size % 4 != 0)
    {
        return std::unexpected(Error::Corrupt);
    }
    inode.checksum_seed = inode_seed;
    return inode;
}

Result<Node> Ext2Filesystem::LoadNode(NodeId node, std::span<std::uint8_t> scratch)
{
    const Result<Inode> inode = LoadInode(node, scratch);
    if (!inode)
    {
        return Failure(inode);
    }
    // A freed inode keeps no links; reserved inodes such as the bad-block list have none either.
    if (inode->link_count == 0)
    {
        return std::unexpected(Error::NotFound);
    }
    const std::optional<NodeType> type = TypeOf(inode->mode);
    if (!type)
    {
        return std::unexpected(Error::Corrupt);
    }
    return Node{.inode = *inode, .type = *type};
}

Result<std::uint32_t> Ext2Filesystem::ReadPointer(std::uint32_t block, std::uint64_t index,
                                                  std::span<std::uint8_t> scratch)
{
    std::array<std::uint8_t, PointerSize> raw{};
    if (const Status read = cache.Read(block, index * PointerSize, raw, scratch); !read)
    {
        return Failure(read);
    }
    return LoadLittleEndian<std::uint32_t>(raw.data());
}

// Each level of indirection multiplies the reach by the pointers a block holds. The path is the
// index into each indirect block on the way down; the depth is fixed, so a corrupt block map
// cannot loop.
std::optional<BlockPath> Ext2Filesystem::PathOf(std::uint64_t logical) const
{
    const std::uint64_t per_block = pointers_per_block;
    const std::uint64_t single_start = DirectBlocks;
    const std::uint64_t double_start = single_start + per_block;
    const std::uint64_t triple_start = double_start + per_block * per_block;
    const std::uint64_t end = triple_start + per_block * per_block * per_block;
    const auto index = [](std::uint64_t value) { return static_cast<std::uint32_t>(value); };
    if (logical < single_start)
    {
        return BlockPath{.root = index(logical), .depth = 0, .index = {}};
    }
    if (logical < double_start)
    {
        return BlockPath{.root = SingleIndirectBlock,
                         .depth = 1,
                         .index = {index(logical - single_start), 0, 0}};
    }
    if (logical < triple_start)
    {
        const std::uint64_t offset = logical - double_start;
        return BlockPath{.root = DoubleIndirectBlock,
                         .depth = 2,
                         .index = {index(offset / per_block), index(offset % per_block), 0}};
    }
    if (logical < end)
    {
        const std::uint64_t offset = logical - triple_start;
        return BlockPath{.root = TripleIndirectBlock,
                         .depth = 3,
                         .index = {index(offset / (per_block * per_block)),
                                   index((offset / per_block) % per_block),
                                   index(offset % per_block)}};
    }
    return std::nullopt;
}

Result<std::uint32_t> Ext2Filesystem::MapBlock(const Inode& inode, std::uint64_t logical,
                                               std::span<std::uint8_t> scratch)
{
    // ext4's extent trees and inline data put something else in i_block, and encrypted contents
    // need keys.
    if ((inode.flags & (InodeExtents | InodeInlineData | InodeEncrypted)) != 0)
    {
        return std::unexpected(Error::Unsupported);
    }
    const std::optional<BlockPath> path = PathOf(logical);
    if (!path)
    {
        // Past the block map's reach, which a size beyond it claims.
        return std::unexpected(Error::Corrupt);
    }
    std::uint32_t block = inode.Block(path->root);
    for (std::uint32_t level = 0; level < path->depth; ++level)
    {
        if (block == 0)
        {
            return 0U;
        }
        if (!IsDataBlock(block))
        {
            return std::unexpected(Error::Corrupt);
        }
        const Result<std::uint32_t> next = ReadPointer(block, path->index[level], scratch);
        if (!next)
        {
            return next;
        }
        block = *next;
    }
    if (block != 0 && !IsDataBlock(block))
    {
        return std::unexpected(Error::Corrupt);
    }
    return block;
}

bool Ext2Filesystem::DirectoryBlockMatches(const Inode& directory, std::uint64_t logical,
                                           std::span<const std::uint8_t> block) const
{
    if (hashed_directories && (directory.flags & InodeIndexed) != 0 && IsIndexBlock(block, logical))
    {
        return IndexBlockChecksumMatches(block, directory.checksum_seed);
    }
    return DirectoryBlockChecksumMatches(block, directory.checksum_seed);
}

Result<NodeType> Ext2Filesystem::EntryType(const RawEntry& entry, std::span<std::uint8_t> scratch)
{
    if (const std::optional<NodeType> type = TypeOfEntry(entry.file_type))
    {
        return *type;
    }
    const Result<Inode> inode = LoadInode(entry.inode, scratch);
    if (!inode)
    {
        return Failure(inode);
    }
    const std::optional<NodeType> type = TypeOf(inode->mode);
    if (!type)
    {
        return std::unexpected(Error::Corrupt);
    }
    return *type;
}

Result<std::size_t> Ext2Filesystem::ReadContents(const Inode& file, std::uint64_t offset,
                                                 std::span<std::uint8_t> buffer,
                                                 std::span<std::uint8_t> scratch)
{
    if (offset >= file.size || buffer.empty())
    {
        return 0U;
    }
    const std::uint64_t remaining = file.size - offset;
    const std::size_t count =
        remaining < buffer.size() ? static_cast<std::size_t>(remaining) : buffer.size();
    std::size_t done = 0;
    while (done < count)
    {
        const std::uint64_t position = offset + done;
        const std::uint64_t logical = position / block_size;
        const auto within = static_cast<std::size_t>(position % block_size);
        const Result<std::uint32_t> physical = MapBlock(file, logical, scratch);
        if (!physical)
        {
            return Failure(physical);
        }
        if (within == 0 && count - done >= block_size)
        {
            // Whole blocks go straight into the caller's buffer, consecutive ones in one read.
            const std::size_t most = (count - done) / block_size;
            std::size_t run = 1;
            while (run < most)
            {
                const Result<std::uint32_t> next = MapBlock(file, logical + run, scratch);
                if (!next)
                {
                    return Failure(next);
                }
                if (*physical == 0 ? *next != 0
                                   : std::uint64_t{*next} != std::uint64_t{*physical} + run)
                {
                    break;
                }
                ++run;
            }
            const std::span<std::uint8_t> target = buffer.subspan(done, run * block_size);
            if (*physical == 0)
            {
                ZeroBytes(target);
            }
            else if (const Status read = cache.ReadUncached(*physical, target); !read)
            {
                return Failure(read);
            }
            done += run * block_size;
            continue;
        }
        const std::size_t part =
            block_size - within < count - done ? block_size - within : count - done;
        if (*physical == 0)
        {
            ZeroBytes(buffer.subspan(done, part));
        }
        else
        {
            if (const Status read = cache.ReadUncached(*physical, scratch); !read)
            {
                return Failure(read);
            }
            CopyBytes(buffer.data() + done, scratch.data() + within, part);
        }
        done += part;
    }
    return count;
}

Capabilities Ext2Filesystem::GetCapabilities() const
{
    return {
        .writable = !read_only,
        .symbolic_links = true,
        .hard_links = true,
        .permissions = true,
        .case_sensitive = true,
        .maximum_name_length = MaximumNameLength,
        .maximum_file_size = MaximumFileSize(),
    };
}

NodeId Ext2Filesystem::Root() const
{
    return RootInode;
}

Result<VolumeStatistics> Ext2Filesystem::Statistics()
{
    const std::uint64_t free_blocks = superblock.free_blocks_count < superblock.blocks_count
                                          ? superblock.free_blocks_count
                                          : superblock.blocks_count;
    const std::uint32_t free_inodes = superblock.free_inodes_count < superblock.inodes_count
                                          ? superblock.free_inodes_count
                                          : superblock.inodes_count;
    return VolumeStatistics{
        .block_size = block_size,
        .total_blocks = superblock.blocks_count,
        .free_blocks = free_blocks,
        .total_nodes = superblock.inodes_count,
        .free_nodes = free_inodes,
    };
}

Result<NodeId> Ext2Filesystem::Lookup(NodeId directory, std::string_view name)
{
    if (name.size() > MaximumNameLength)
    {
        return std::unexpected(Error::NameTooLong);
    }
    Buffers buffers(block_size, 2);
    if (buffers.Empty())
    {
        return std::unexpected(Error::OutOfMemory);
    }
    const Result<Node> node = LoadNode(directory, buffers[1]);
    if (!node)
    {
        return Failure(node);
    }
    if (node->type != NodeType::Directory)
    {
        return std::unexpected(Error::NotDirectory);
    }
    // ".." is an entry like any other, which in the root names the root itself.
    if (name == ".")
    {
        return directory;
    }
    NodeId found = 0;
    const Result<bool> scanned =
        WalkDirectory(node->inode, 0, buffers[0], buffers[1],
                      [&](const RawEntry& entry, const RecordPlace&) -> Result<bool>
                      {
                          if (entry.inode == 0 || entry.name != name)
                          {
                              return false;
                          }
                          found = entry.inode;
                          return true;
                      });
    if (!scanned)
    {
        return Failure(scanned);
    }
    if (!*scanned)
    {
        return std::unexpected(Error::NotFound);
    }
    return found;
}

Result<bool> Ext2Filesystem::ReadDirectory(NodeId directory, std::uint64_t& cookie,
                                           DirectoryEntry& entry)
{
    Buffers buffers(block_size, 2);
    if (buffers.Empty())
    {
        return std::unexpected(Error::OutOfMemory);
    }
    const Result<Node> node = LoadNode(directory, buffers[1]);
    if (!node)
    {
        return Failure(node);
    }
    return WalkDirectory(node->inode, cookie, buffers[0], buffers[1],
                         [&](const RawEntry& raw, const RecordPlace& place) -> Result<bool>
                         {
                             if (raw.inode == 0 || raw.name == "." || raw.name == "..")
                             {
                                 return false;
                             }
                             const Result<NodeType> type = EntryType(raw, buffers[1]);
                             if (!type)
                             {
                                 return Failure(type);
                             }
                             entry.node = raw.inode;
                             entry.type = *type;
                             entry.name_length = static_cast<std::uint16_t>(raw.name.size());
                             CopyBytes(entry.name.data(), raw.name.data(), raw.name.size());
                             cookie = place.next;
                             return true;
                         });
}

Result<Attributes> Ext2Filesystem::GetAttributes(NodeId node)
{
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
    const Inode& inode = loaded->inode;
    return Attributes{
        .node = node,
        .type = loaded->type,
        .size = inode.size,
        .mode = std::uint32_t{inode.mode} & ModePermissions,
        .owner = inode.owner,
        .group = inode.group,
        .link_count = inode.link_count,
        .access_time = inode.access_time,
        .modify_time = inode.modify_time,
        .change_time = inode.change_time,
    };
}

Result<std::size_t> Ext2Filesystem::Read(NodeId file, std::uint64_t offset,
                                         std::span<std::uint8_t> buffer)
{
    Buffers buffers(block_size, 1);
    if (buffers.Empty())
    {
        return std::unexpected(Error::OutOfMemory);
    }
    const Result<Node> node = LoadNode(file, buffers[0]);
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
    return ReadContents(node->inode, offset, buffer, buffers[0]);
}

Result<std::size_t> Ext2Filesystem::ReadLink(NodeId link, std::span<char> buffer)
{
    Buffers buffers(block_size, 1);
    if (buffers.Empty())
    {
        return std::unexpected(Error::OutOfMemory);
    }
    const Result<Node> node = LoadNode(link, buffers[0]);
    if (!node)
    {
        return Failure(node);
    }
    if (node->type != NodeType::SymbolicLink)
    {
        return std::unexpected(Error::InvalidArgument);
    }
    const Inode& inode = node->inode;
    if ((inode.flags & (InodeInlineData | InodeEncrypted)) != 0)
    {
        return std::unexpected(Error::Unsupported);
    }
    const std::uint64_t size = inode.size;
    const std::size_t count = size < buffer.size() ? static_cast<std::size_t>(size) : buffer.size();
    // A target shorter than i_block is kept there ("fast"); a longer one fills the link's only
    // block ("slow").
    if (size < BlockArrayBytes)
    {
        CopyBytes(buffer.data(), inode.blocks.data(), count);
        return count;
    }
    if (size > block_size)
    {
        return std::unexpected(Error::Corrupt);
    }
    const Result<std::uint32_t> block = MapBlock(inode, 0, buffers[0]);
    if (!block)
    {
        return Failure(block);
    }
    if (*block == 0)
    {
        return std::unexpected(Error::Corrupt);
    }
    if (const Status read = cache.ReadUncached(*block, buffers[0]); !read)
    {
        return Failure(read);
    }
    CopyBytes(buffer.data(), buffers[0].data(), count);
    return count;
}

namespace
{

std::string_view Ext2FilesystemType::Name() const
{
    return "ext2";
}

bool Ext2FilesystemType::Probe(BlockDevice& volume)
{
    const Result<Superblock> superblock = ReadSuperblock(volume);
    return superblock && LooksLikeExt(*superblock);
}

Result<Filesystem*> Ext2FilesystemType::Mount(BlockDevice& volume, MountOptions options)
{
    const Result<Superblock> superblock = ReadSuperblock(volume);
    if (!superblock)
    {
        return Failure(superblock);
    }
    if (!LooksLikeExt(*superblock))
    {
        return std::unexpected(Error::Unsupported);
    }
    if (const std::uint32_t unknown = superblock->incompatible & ~IncompatibleReadable;
        unknown != 0)
    {
        LogWithValue("ext2: unsupported incompatible features ", unknown);
        return std::unexpected(Error::Unsupported);
    }
    if (const std::uint32_t unreadable =
            superblock->read_only_compatible & ReadOnlyCompatibleUnreadable;
        unreadable != 0)
    {
        LogWithValue("ext2: unsupported read-only compatible features ", unreadable);
        return std::unexpected(Error::Unsupported);
    }
    if (const Status checked =
            CheckSuperblock(*superblock, volume.SectorCount() * volume.SectorSize());
        !checked)
    {
        if (checked.error() == Error::Corrupt && !superblock->checksum_matches)
        {
            LogWarning("ext2: ", "the superblock's checksum does not match");
        }
        else if (checked.error() == Error::Unsupported)
        {
            LogWithValue("ext2: unsupported revision, block size or checksum type, log2 of KiB ",
                         superblock->log_block_size);
        }
        else
        {
            LogWithValue("ext2: inconsistent superblock, block count ",
                         static_cast<std::uint32_t>(superblock->blocks_count));
        }
        return Failure(checked);
    }
    if (superblock->block_size % volume.SectorSize() != 0)
    {
        LogWithValue("ext2: blocks smaller than the device's sectors, block size ",
                     superblock->block_size);
        return std::unexpected(Error::Unsupported);
    }

    bool read_only = options.read_only;
    // Linux replays the journal first; without that, reads see the volume as it was before the
    // transactions the journal still holds.
    if ((superblock->incompatible & IncompatibleRecover) != 0)
    {
        LogWarning("ext2: mounted read-only: ",
                   "the journal needs recovery, which is not replayed; recent changes are missing");
        read_only = true;
    }
    if (const std::optional<std::string_view> reason = WriteRefusal(*superblock);
        reason && !read_only)
    {
        LogWarning("ext2: mounted read-only: ", *reason);
        read_only = true;
    }
    void* memory = Host::Allocate(sizeof(Ext2Filesystem));
    if (memory == nullptr)
    {
        return std::unexpected(Error::OutOfMemory);
    }
    auto* filesystem = new (memory) Ext2Filesystem(volume, *superblock, read_only);
    if (const Status initialized = filesystem->Initialize(); !initialized)
    {
        filesystem->~Ext2Filesystem();
        Host::Free(memory);
        return Failure(initialized);
    }
    return filesystem;
}

Status Ext2FilesystemType::Unmount(Filesystem& filesystem)
{
    auto& ext2 = static_cast<Ext2Filesystem&>(filesystem);
    if (const Status synced = ext2.Sync(); !synced)
    {
        return synced;
    }
    ext2.~Ext2Filesystem();
    Host::Free(&ext2);
    return {};
}

constinit Ext2FilesystemType ext2_type;

} // namespace

FilesystemType& Ext2Type()
{
    return ext2_type;
}

Result<ExtFeatures> DescribeFeatures(BlockDevice& volume)
{
    const Result<Superblock> superblock = ReadSuperblock(volume);
    if (!superblock)
    {
        return Failure(superblock);
    }
    if (!LooksLikeExt(*superblock))
    {
        return std::unexpected(Error::Unsupported);
    }
    return ExtFeatures{
        .variant = Variant(*superblock),
        .compatible = superblock->compatible,
        .incompatible = superblock->incompatible,
        .read_only_compatible = superblock->read_only_compatible,
    };
}

} // namespace FeatherDisk::Ext
