// SPDX-License-Identifier: MIT OR Apache-2.0
#include "ext/inode.h"

#include "common/little_endian.h"
#include "ext/superblock.h"

namespace FeatherDisk::Ext
{
namespace
{

constexpr std::uint32_t MaximumNanoseconds = 999'999'999;

std::uint16_t Load16(const std::uint8_t* bytes)
{
    return LoadLittleEndian<std::uint16_t>(bytes);
}

std::uint32_t Load32(const std::uint8_t* bytes)
{
    return LoadLittleEndian<std::uint32_t>(bytes);
}

// The 32-bit seconds are signed. Large inodes extend each time with a word whose low two bits
// add to the seconds' bits 32 and 33 (past 2038) and whose other 30 bits are nanoseconds.
Timestamp DecodeTime(std::uint32_t seconds, std::optional<std::uint32_t> extra)
{
    Timestamp time = {.seconds = static_cast<std::int32_t>(seconds), .nanoseconds = 0};
    if (extra)
    {
        time.seconds += static_cast<std::int64_t>(*extra & 3U) << 32;
        time.nanoseconds = *extra >> 2;
        if (time.nanoseconds > MaximumNanoseconds)
        {
            time.nanoseconds = MaximumNanoseconds;
        }
    }
    return time;
}

} // namespace

std::uint32_t Inode::Block(std::uint32_t index) const
{
    return Load32(blocks.data() + static_cast<std::size_t>(index) * 4);
}

Inode DecodeInode(std::span<const std::uint8_t> raw, std::uint32_t read_only_compatible,
                  std::uint32_t block_size)
{
    const std::uint8_t* bytes = raw.data();
    const std::uint16_t mode = Load16(bytes + 0);
    const std::uint32_t flags = Load32(bytes + 32);
    const bool huge_files = (read_only_compatible & ReadOnlyCompatibleHugeFile) != 0;

    // A regular file's size has its upper 32 bits where revision 0 kept the directory ACL.
    std::uint64_t size = Load32(bytes + 4);
    if ((mode & ModeTypeMask) == ModeRegular)
    {
        size |= std::uint64_t{Load32(bytes + 108)} << 32;
    }
    std::uint64_t blocks = Load32(bytes + 28);
    std::uint64_t block_unit = 512;
    if (huge_files)
    {
        blocks |= std::uint64_t{Load16(bytes + 116)} << 32;
        if ((flags & InodeHugeFile) != 0)
        {
            block_unit = block_size;
        }
    }

    // i_extra_isize says how much of the space past 128 bytes holds fields.
    std::uint32_t extra_size = 0;
    if (raw.size() >= GoodOldInodeSize + 2)
    {
        extra_size = Load16(bytes + GoodOldInodeSize);
    }
    const auto extra = [&](std::size_t offset) -> std::optional<std::uint32_t>
    {
        if (offset + 4 > GoodOldInodeSize + extra_size || offset + 4 > raw.size())
        {
            return std::nullopt;
        }
        return Load32(bytes + offset);
    };

    Inode inode = {
        .mode = mode,
        .owner = std::uint32_t{Load16(bytes + 2)} | (std::uint32_t{Load16(bytes + 120)} << 16),
        .group = std::uint32_t{Load16(bytes + 24)} | (std::uint32_t{Load16(bytes + 122)} << 16),
        .size = size,
        .link_count = Load16(bytes + 26),
        .flags = flags,
        .allocated_bytes = blocks * block_unit,
        .access_time = DecodeTime(Load32(bytes + 8), extra(140)),
        .modify_time = DecodeTime(Load32(bytes + 16), extra(136)),
        .change_time = DecodeTime(Load32(bytes + 12), extra(132)),
        .blocks = {},
    };
    for (std::size_t index = 0; index < BlockArrayBytes; ++index)
    {
        inode.blocks[index] = bytes[40 + index];
    }
    return inode;
}

std::optional<NodeType> TypeOf(std::uint16_t mode)
{
    switch (mode & ModeTypeMask)
    {
    case ModeRegular:
        return NodeType::File;
    case ModeDirectory:
        return NodeType::Directory;
    case ModeSymbolicLink:
        return NodeType::SymbolicLink;
    case ModeCharacterDevice:
    case ModeBlockDevice:
    case ModeFifo:
    case ModeSocket:
        return NodeType::Special;
    default:
        return std::nullopt;
    }
}

} // namespace FeatherDisk::Ext
