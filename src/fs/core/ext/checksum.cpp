// SPDX-License-Identifier: MIT OR Apache-2.0
#include "ext/checksum.h"

#include "common/crc.h"
#include "common/little_endian.h"

#include <array>

namespace FeatherDisk::Ext
{
namespace
{

// Each checksum field counts as zeros while its structure is summed.
constexpr std::array<std::uint8_t, 2> ZeroField{};
constexpr std::size_t DescriptorChecksumOffset = 30;
constexpr std::size_t InodeChecksumLowOffset = 124;
constexpr std::size_t InodeChecksumHighOffset = 130;
constexpr std::uint8_t DirectoryTailFileType = 0xDE;
// An index block's entries are 8 bytes (hash and block); its checksum record follows the limit:
// 4 reserved bytes, then the CRC32C.
constexpr std::size_t IndexEntrySize = 8;
constexpr std::size_t IndexTailSize = 8;
constexpr std::size_t IndexNodeCountOffset = 8;
constexpr std::size_t IndexRootCountOffset = 32;
constexpr std::size_t IndexRootInfoOffset = 24;
constexpr std::uint8_t IndexRootInfoLength = 8;

std::array<std::uint8_t, 4> LittleEndian32(std::uint32_t value)
{
    std::array<std::uint8_t, 4> bytes{};
    StoreLittleEndian(bytes.data(), value);
    return bytes;
}

std::uint16_t Load16(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return LoadLittleEndian<std::uint16_t>(bytes.data() + offset);
}

std::uint32_t Load32(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return LoadLittleEndian<std::uint32_t>(bytes.data() + offset);
}

} // namespace

std::uint32_t VolumeChecksumSeed(const Superblock& superblock)
{
    if ((superblock.incompatible & IncompatibleChecksumSeed) != 0)
    {
        return superblock.stored_checksum_seed;
    }
    return Crc32c(~0U, superblock.uuid);
}

std::uint16_t GroupDescriptorChecksum(const Superblock& superblock, std::uint32_t volume_seed,
                                      std::uint32_t group, std::span<const std::uint8_t> bytes)
{
    const std::array<std::uint8_t, 4> number = LittleEndian32(group);
    const std::span<const std::uint8_t> before = bytes.first(DescriptorChecksumOffset);
    const std::span<const std::uint8_t> after =
        bytes.subspan(DescriptorChecksumOffset + ZeroField.size());
    if (HasMetadataChecksums(superblock))
    {
        std::uint32_t crc = Crc32c(volume_seed, number);
        crc = Crc32c(crc, before);
        crc = Crc32c(crc, ZeroField);
        crc = Crc32c(crc, after);
        return static_cast<std::uint16_t>(crc & 0xFFFFU);
    }
    if (!HasGroupChecksums(superblock))
    {
        return 0;
    }
    std::uint16_t crc = Crc16(0xFFFF, superblock.uuid);
    crc = Crc16(crc, number);
    crc = Crc16(crc, before);
    return Crc16(crc, after);
}

std::uint32_t BitmapChecksum(std::uint32_t volume_seed, std::span<const std::uint8_t> bitmap)
{
    return Crc32c(volume_seed, bitmap);
}

std::uint32_t InodeChecksumSeed(std::uint32_t volume_seed, std::uint32_t inode,
                                std::uint32_t generation)
{
    return Crc32c(Crc32c(volume_seed, LittleEndian32(inode)), LittleEndian32(generation));
}

bool InodeChecksumMatches(std::span<const std::uint8_t> raw, std::uint32_t inode_seed)
{
    std::uint32_t crc = Crc32c(inode_seed, raw.first(InodeChecksumLowOffset));
    crc = Crc32c(crc, ZeroField);
    crc = Crc32c(crc, raw.subspan(InodeChecksumLowOffset + 2,
                                  GoodOldInodeSize - InodeChecksumLowOffset - 2));
    std::uint32_t stored = Load16(raw, InodeChecksumLowOffset);
    bool high_half = false;
    if (raw.size() > GoodOldInodeSize)
    {
        // i_extra_isize itself, then i_checksum_hi when the extra fields reach it.
        crc = Crc32c(crc, raw.subspan(GoodOldInodeSize, 2));
        std::size_t offset = InodeChecksumHighOffset;
        high_half = GoodOldInodeSize + Load16(raw, GoodOldInodeSize) >= InodeChecksumHighOffset + 2;
        if (high_half)
        {
            crc = Crc32c(crc, ZeroField);
            offset += 2;
            stored |= std::uint32_t{Load16(raw, InodeChecksumHighOffset)} << 16;
        }
        crc = Crc32c(crc, raw.subspan(offset));
    }
    if (!high_half)
    {
        crc &= 0xFFFFU;
    }
    return crc == stored;
}

bool DirectoryBlockChecksumMatches(std::span<const std::uint8_t> block, std::uint32_t inode_seed)
{
    if (block.size() < DirectoryTailSize)
    {
        return false;
    }
    const std::size_t tail = block.size() - DirectoryTailSize;
    if (Load32(block, tail) != 0 || Load16(block, tail + 4) != DirectoryTailSize ||
        block[tail + 6] != 0 || block[tail + 7] != DirectoryTailFileType)
    {
        return false;
    }
    return Crc32c(inode_seed, block.first(tail)) == Load32(block, tail + 8);
}

bool IsIndexBlock(std::span<const std::uint8_t> block, std::uint64_t logical)
{
    return logical == 0 || (Load32(block, 0) == 0 && Load16(block, 4) == block.size());
}

bool IndexBlockChecksumMatches(std::span<const std::uint8_t> block, std::uint32_t inode_seed)
{
    const std::size_t size = block.size();
    std::size_t count_offset = 0;
    if (Load16(block, 4) == size)
    {
        count_offset = IndexNodeCountOffset;
    }
    else if (Load16(block, 4) == 12 && Load16(block, 12 + 4) == size - 12 &&
             Load32(block, IndexRootInfoOffset) == 0 &&
             block[IndexRootInfoOffset + 5] == IndexRootInfoLength)
    {
        count_offset = IndexRootCountOffset;
    }
    else
    {
        return false;
    }
    const std::size_t limit = Load16(block, count_offset);
    const std::size_t count = Load16(block, count_offset + 2);
    const std::size_t tail = count_offset + limit * IndexEntrySize;
    if (count > limit || tail > size - IndexTailSize)
    {
        return false;
    }
    std::uint32_t crc = Crc32c(inode_seed, block.first(count_offset + count * IndexEntrySize));
    crc = Crc32c(crc, block.subspan(tail, 4));
    crc = Crc32c(crc, std::array<std::uint8_t, 4>{});
    return crc == Load32(block, tail + 4);
}

} // namespace FeatherDisk::Ext
