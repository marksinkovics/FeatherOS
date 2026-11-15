// SPDX-License-Identifier: MIT OR Apache-2.0
#include "fs/ext2.h"

#include "common/block_cache.h"
#include "common/bytes.h"
#include "common/host_buffer.h"
#include "common/little_endian.h"
#include "ext/directory.h"
#include "ext/inode.h"
#include "ext/superblock.h"
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

// Writing is not implemented yet, so every mount is read-only.
constexpr bool WritesSupported = false;
// Metadata blocks each mounted volume keeps in memory.
constexpr std::size_t CacheBytes = 32 * 1024;
constexpr std::uint32_t PointerSize = 4;
// i_blocks counts 512-byte sectors in 32 bits without huge_file, which caps a file at 2 TiB;
// without large_file, sizes stay below 2 GiB.
constexpr std::uint64_t MaximumAllocatedBytes = std::uint64_t{0xFFFFFFFF} * 512;
constexpr std::uint64_t MaximumSmallFileBytes = 0x7FFFFFFF;

template <typename T> std::unexpected<Error> Failure(const Result<T>& result)
{
    return std::unexpected(result.error());
}

std::uint64_t DivideRoundingUp(std::uint64_t value, std::uint64_t divisor)
{
    return value / divisor + (value % divisor != 0 ? 1 : 0);
}

// The message, then value as eight hexadecimal digits.
void LogWithValue(std::string_view message, std::uint32_t value)
{
    constexpr std::string_view Digits = "0123456789abcdef";
    std::array<char, 96> text{};
    std::size_t length = 0;
    for (const char character : message)
    {
        if (length < text.size() - 10)
        {
            text[length++] = character;
        }
    }
    text[length++] = '0';
    text[length++] = 'x';
    for (int shift = 28; shift >= 0; shift -= 4)
    {
        text[length++] = Digits[(value >> shift) & 0xFU];
    }
    Host::Log(Host::LogLevel::Warning, std::string_view(text.data(), length));
}

// Block-sized buffers from the host, for one operation.
class Buffers
{
public:
    Buffers(std::uint32_t block_size, std::size_t count);

    bool Empty() const;
    std::span<std::uint8_t> operator[](std::size_t index);

private:
    HostBuffer memory;
    std::uint32_t size;
};

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

// An inode in use, of a type ext2 defines.
struct Node
{
    Inode inode;
    NodeType type;
};

class Ext2Filesystem final : public Filesystem
{
public:
    Ext2Filesystem(BlockDevice& device, const Superblock& layout, bool mounted_read_only);
    ~Ext2Filesystem() = default;

    // Constructed in memory from Host::Allocate; the core has no <new> for the global form.
    static void* operator new(std::size_t size, void* place) noexcept;

    Status Initialize();

    Capabilities GetCapabilities() const override;
    NodeId Root() const override;
    Result<VolumeStatistics> Statistics() override;
    Result<NodeId> Lookup(NodeId directory, std::string_view name) override;
    Result<bool> ReadDirectory(NodeId directory, std::uint64_t& cookie,
                               DirectoryEntry& entry) override;
    Result<Attributes> GetAttributes(NodeId node) override;
    Status SetAttributes(NodeId node, const AttributeChanges& changes) override;
    Result<std::size_t> Read(NodeId file, std::uint64_t offset,
                             std::span<std::uint8_t> buffer) override;
    Result<std::size_t> Write(NodeId file, std::uint64_t offset,
                              std::span<const std::uint8_t> data) override;
    Status Truncate(NodeId file, std::uint64_t size) override;
    Result<NodeId> Create(NodeId directory, std::string_view name, std::uint32_t mode) override;
    Result<NodeId> MakeDirectory(NodeId directory, std::string_view name,
                                 std::uint32_t mode) override;
    Status Remove(NodeId directory, std::string_view name) override;
    Status RemoveDirectory(NodeId directory, std::string_view name) override;
    Status Rename(NodeId from_directory, std::string_view from_name, NodeId to_directory,
                  std::string_view to_name) override;
    Status Link(NodeId node, NodeId directory, std::string_view name) override;
    Result<NodeId> SymbolicLink(NodeId directory, std::string_view name,
                                std::string_view target) override;
    Result<std::size_t> ReadLink(NodeId link, std::span<char> buffer) override;
    Status Sync() override;

private:
    std::unexpected<Error> Refused() const;
    bool IsDataBlock(std::uint64_t block) const;
    Result<Inode> LoadInode(NodeId node, std::span<std::uint8_t> scratch);
    Result<Node> LoadNode(NodeId node, std::span<std::uint8_t> scratch);
    Result<std::uint32_t> ReadPointer(std::uint32_t block, std::uint64_t index,
                                      std::span<std::uint8_t> scratch);
    // The block holding a file's logical block, or 0 for a hole.
    Result<std::uint32_t> MapBlock(const Inode& inode, std::uint64_t logical,
                                   std::span<std::uint8_t> scratch);
    // Calls visit(entry, offset of the next entry) for each used entry from byte start on,
    // until it returns true. block receives each directory block, and scratch serves the rest.
    template <typename Visit>
    Result<bool> ScanDirectory(const Inode& directory, std::uint64_t start,
                               std::span<std::uint8_t> block, std::span<std::uint8_t> scratch,
                               Visit visit);
    Result<NodeType> EntryType(const RawEntry& entry, std::span<std::uint8_t> scratch);
    Result<std::size_t> ReadContents(const Inode& file, std::uint64_t offset,
                                     std::span<std::uint8_t> buffer,
                                     std::span<std::uint8_t> scratch);

    BlockDevice& volume;
    Superblock superblock;
    BlockCache cache;
    bool read_only;
    bool has_file_type;
    std::uint32_t block_size;
    std::uint32_t pointers_per_block;
    std::uint64_t inode_table_blocks;
};

class Ext2FilesystemType final : public FilesystemType
{
public:
    constexpr Ext2FilesystemType() = default;

    std::string_view Name() const override;
    bool Probe(BlockDevice& volume) override;
    Result<Filesystem*> Mount(BlockDevice& volume, MountOptions options) override;
    Status Unmount(Filesystem& filesystem) override;
};

Ext2Filesystem::Ext2Filesystem(BlockDevice& device, const Superblock& layout,
                               bool mounted_read_only)
    : volume(device), superblock(layout), read_only(mounted_read_only),
      has_file_type((layout.incompatible & IncompatibleFileType) != 0),
      block_size(layout.block_size), pointers_per_block(layout.block_size / PointerSize),
      inode_table_blocks(DivideRoundingUp(
          std::uint64_t{layout.inodes_per_group} * layout.inode_size, layout.block_size))
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

Result<Inode> Ext2Filesystem::LoadInode(NodeId node, std::span<std::uint8_t> scratch)
{
    if (node == 0 || node > superblock.inodes_count)
    {
        return std::unexpected(Error::InvalidArgument);
    }
    // Inodes count from 1, in groups of inodes_per_group, each group with its own inode table
    // that its 32-byte descriptor locates.
    const auto number = static_cast<std::uint32_t>(node - 1);
    const std::uint32_t group = number / superblock.inodes_per_group;
    const std::uint32_t index = number % superblock.inodes_per_group;
    const std::uint64_t descriptor = std::uint64_t{group} * GroupDescriptorSize;
    std::array<std::uint8_t, GroupDescriptorSize> raw_descriptor{};
    if (const Status read = cache.Read(superblock.first_data_block + 1 + descriptor / block_size,
                                       descriptor % block_size, raw_descriptor, scratch);
        !read)
    {
        return Failure(read);
    }
    const std::uint32_t table = LoadLittleEndian<std::uint32_t>(raw_descriptor.data() + 8);
    if (!IsDataBlock(table) || table + inode_table_blocks > superblock.blocks_count)
    {
        return std::unexpected(Error::Corrupt);
    }

    const std::uint64_t byte = std::uint64_t{index} * superblock.inode_size;
    std::array<std::uint8_t, DecodedInodeBytes> raw{};
    const std::size_t length =
        superblock.inode_size < DecodedInodeBytes ? superblock.inode_size : DecodedInodeBytes;
    const std::span<std::uint8_t> bytes(raw.data(), length);
    if (const Status read =
            cache.Read(table + byte / block_size, byte % block_size, bytes, scratch);
        !read)
    {
        return Failure(read);
    }
    return DecodeInode(bytes, superblock.read_only_compatible, block_size);
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

Result<std::uint32_t> Ext2Filesystem::MapBlock(const Inode& inode, std::uint64_t logical,
                                               std::span<std::uint8_t> scratch)
{
    // ext4's extent trees and inline data put something else in i_block.
    if ((inode.flags & (InodeExtents | InodeInlineData)) != 0)
    {
        return std::unexpected(Error::Unsupported);
    }
    // Each level of indirection multiplies the reach by the pointers a block holds. The path is
    // the index into each indirect block on the way down; the depth is fixed, so a corrupt block
    // map cannot loop.
    const std::uint64_t per_block = pointers_per_block;
    const std::uint64_t single_start = DirectBlocks;
    const std::uint64_t double_start = single_start + per_block;
    const std::uint64_t triple_start = double_start + per_block * per_block;
    const std::uint64_t end = triple_start + per_block * per_block * per_block;
    std::array<std::uint64_t, 3> path{};
    std::uint32_t depth = 0;
    std::uint32_t block = 0;
    if (logical < single_start)
    {
        block = inode.Block(static_cast<std::uint32_t>(logical));
    }
    else if (logical < double_start)
    {
        block = inode.Block(SingleIndirectBlock);
        path = {logical - single_start, 0, 0};
        depth = 1;
    }
    else if (logical < triple_start)
    {
        const std::uint64_t index = logical - double_start;
        block = inode.Block(DoubleIndirectBlock);
        path = {index / per_block, index % per_block, 0};
        depth = 2;
    }
    else if (logical < end)
    {
        const std::uint64_t index = logical - triple_start;
        block = inode.Block(TripleIndirectBlock);
        path = {index / (per_block * per_block), (index / per_block) % per_block,
                index % per_block};
        depth = 3;
    }
    else
    {
        // Past the block map's reach, which a size beyond it claims.
        return std::unexpected(Error::Corrupt);
    }
    for (std::uint32_t level = 0; level < depth; ++level)
    {
        if (block == 0)
        {
            return 0U;
        }
        if (!IsDataBlock(block))
        {
            return std::unexpected(Error::Corrupt);
        }
        const Result<std::uint32_t> next = ReadPointer(block, path[level], scratch);
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

template <typename Visit>
Result<bool> Ext2Filesystem::ScanDirectory(const Inode& directory, std::uint64_t start,
                                           std::span<std::uint8_t> block,
                                           std::span<std::uint8_t> scratch, Visit visit)
{
    if (TypeOf(directory.mode) != NodeType::Directory)
    {
        return std::unexpected(Error::NotDirectory);
    }
    // Directories are whole blocks without holes, so their size cannot exceed what they hold;
    // the check also keeps a corrupt size from sending the scan over billions of blocks.
    if (directory.size % block_size != 0 || directory.size > directory.allocated_bytes)
    {
        return std::unexpected(Error::Corrupt);
    }
    const std::uint64_t blocks = directory.size / block_size;
    for (std::uint64_t logical = start / block_size; logical < blocks; ++logical)
    {
        const Result<std::uint32_t> physical = MapBlock(directory, logical, scratch);
        if (!physical)
        {
            return Failure(physical);
        }
        if (*physical == 0)
        {
            continue;
        }
        if (const Status read = cache.Read(*physical, 0, block, block); !read)
        {
            return Failure(read);
        }
        // Entries are walked from the block's start even when resuming, so a cookie that does
        // not fall on an entry still finds the next one.
        for (std::size_t offset = 0; offset < block_size;)
        {
            const Result<RawEntry> entry =
                ParseEntry(block, offset, has_file_type, superblock.inodes_count);
            if (!entry)
            {
                return Failure(entry);
            }
            const std::uint64_t position = logical * block_size + offset;
            offset += entry->record_length;
            if (entry->inode == 0 || position < start)
            {
                continue;
            }
            const Result<bool> done = visit(*entry, logical * block_size + offset);
            if (!done || *done)
            {
                return done;
            }
        }
    }
    return false;
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
    const std::uint64_t per_block = pointers_per_block;
    const std::uint64_t mapped_blocks =
        DirectBlocks + per_block + per_block * per_block + per_block * per_block * per_block;
    std::uint64_t maximum = mapped_blocks * block_size;
    if (maximum > MaximumAllocatedBytes)
    {
        maximum = MaximumAllocatedBytes;
    }
    if ((superblock.read_only_compatible & ReadOnlyCompatibleLargeFile) == 0 &&
        maximum > MaximumSmallFileBytes)
    {
        maximum = MaximumSmallFileBytes;
    }
    return {
        .writable = !read_only,
        .symbolic_links = true,
        .hard_links = true,
        .permissions = true,
        .case_sensitive = true,
        .maximum_name_length = MaximumNameLength,
        .maximum_file_size = maximum,
    };
}

NodeId Ext2Filesystem::Root() const
{
    return RootInode;
}

Result<VolumeStatistics> Ext2Filesystem::Statistics()
{
    const std::uint32_t free_blocks = superblock.free_blocks_count < superblock.blocks_count
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
        ScanDirectory(node->inode, 0, buffers[0], buffers[1],
                      [&](const RawEntry& entry, std::uint64_t) -> Result<bool>
                      {
                          if (entry.name != name)
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
    return ScanDirectory(node->inode, cookie, buffers[0], buffers[1],
                         [&](const RawEntry& raw, std::uint64_t next) -> Result<bool>
                         {
                             if (raw.name == "." || raw.name == "..")
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
                             cookie = next;
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

Status Ext2Filesystem::SetAttributes(NodeId /*node*/, const AttributeChanges& /*changes*/)
{
    return Refused();
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

Result<std::size_t> Ext2Filesystem::Write(NodeId /*file*/, std::uint64_t /*offset*/,
                                          std::span<const std::uint8_t> /*data*/)
{
    return Refused();
}

Status Ext2Filesystem::Truncate(NodeId /*file*/, std::uint64_t /*size*/)
{
    return Refused();
}

Result<NodeId> Ext2Filesystem::Create(NodeId /*directory*/, std::string_view /*name*/,
                                      std::uint32_t /*mode*/)
{
    return Refused();
}

Result<NodeId> Ext2Filesystem::MakeDirectory(NodeId /*directory*/, std::string_view /*name*/,
                                             std::uint32_t /*mode*/)
{
    return Refused();
}

Status Ext2Filesystem::Remove(NodeId /*directory*/, std::string_view /*name*/)
{
    return Refused();
}

Status Ext2Filesystem::RemoveDirectory(NodeId /*directory*/, std::string_view /*name*/)
{
    return Refused();
}

Status Ext2Filesystem::Rename(NodeId /*from_directory*/, std::string_view /*from_name*/,
                              NodeId /*to_directory*/, std::string_view /*to_name*/)
{
    return Refused();
}

Status Ext2Filesystem::Link(NodeId /*node*/, NodeId /*directory*/, std::string_view /*name*/)
{
    return Refused();
}

Result<NodeId> Ext2Filesystem::SymbolicLink(NodeId /*directory*/, std::string_view /*name*/,
                                            std::string_view /*target*/)
{
    return Refused();
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
    if ((inode.flags & InodeInlineData) != 0)
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

// Nothing is ever written, so there is nothing to hold back.
Status Ext2Filesystem::Sync()
{
    return {};
}

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
    if (const std::uint32_t unknown = superblock->incompatible & ~IncompatibleSupported;
        unknown != 0)
    {
        LogWithValue("ext2: unsupported incompatible features ", unknown);
        return std::unexpected(Error::Unsupported);
    }
    if (const Status checked =
            CheckSuperblock(*superblock, volume.SectorCount() * volume.SectorSize());
        !checked)
    {
        LogWithValue(checked.error() == Error::Unsupported
                         ? "ext2: unsupported revision or block size, log2 of KiB "
                         : "ext2: inconsistent superblock, block count ",
                     checked.error() == Error::Unsupported ? superblock->log_block_size
                                                           : superblock->blocks_count);
        return Failure(checked);
    }
    if (superblock->block_size % volume.SectorSize() != 0)
    {
        LogWithValue("ext2: blocks smaller than the device's sectors, block size ",
                     superblock->block_size);
        return std::unexpected(Error::Unsupported);
    }

    const bool read_only = !WritesSupported || options.read_only ||
                           (superblock->read_only_compatible & ~ReadOnlyCompatibleUnderstood) != 0;
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

} // namespace FeatherDisk::Ext
