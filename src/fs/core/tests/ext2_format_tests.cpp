// SPDX-License-Identifier: MIT OR Apache-2.0
// The ext2 on-disk formats, decoded from bytes built here rather than by mke2fs.
#include <catch2/catch_test_macros.hpp>

#include "common/little_endian.h"
#include "ext/directory.h"
#include "ext/inode.h"
#include "ext/superblock.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <random>
#include <span>
#include <string_view>
#include <vector>

namespace
{

using FeatherDisk::Error;
using FeatherDisk::NodeType;
using FeatherDisk::Result;
using FeatherDisk::StoreLittleEndian;
using namespace FeatherDisk::Ext;

void PutEntry(std::vector<std::uint8_t>& block, std::size_t offset, std::uint32_t inode,
              std::uint16_t record_length, std::string_view name, std::uint8_t file_type)
{
    StoreLittleEndian(block.data() + offset, inode);
    StoreLittleEndian(block.data() + offset + 4, record_length);
    block[offset + 6] = static_cast<std::uint8_t>(name.size());
    block[offset + 7] = file_type;
    std::memcpy(block.data() + offset + 8, name.data(), name.size());
}

// "." and "..", then "hello.txt" filling the rest of a 1 KiB block.
std::vector<std::uint8_t> SmallDirectory()
{
    std::vector<std::uint8_t> block(1024);
    PutEntry(block, 0, 2, 12, ".", 2);
    PutEntry(block, 12, 2, 12, "..", 2);
    PutEntry(block, 24, 12, 1000, "hello.txt", 1);
    return block;
}

std::array<std::uint8_t, SuperblockSize> SuperblockBytes()
{
    std::array<std::uint8_t, SuperblockSize> bytes{};
    std::uint8_t* raw = bytes.data();
    StoreLittleEndian<std::uint32_t>(raw + 0, 2048);  // inodes
    StoreLittleEndian<std::uint32_t>(raw + 4, 8192);  // blocks
    StoreLittleEndian<std::uint32_t>(raw + 12, 1000); // free blocks
    StoreLittleEndian<std::uint32_t>(raw + 16, 2000); // free inodes
    StoreLittleEndian<std::uint32_t>(raw + 20, 1);    // first data block
    StoreLittleEndian<std::uint32_t>(raw + 24, 0);    // 1 KiB blocks
    StoreLittleEndian<std::uint32_t>(raw + 32, 8192); // blocks per group
    StoreLittleEndian<std::uint32_t>(raw + 40, 2048); // inodes per group
    StoreLittleEndian<std::uint16_t>(raw + 56, 0xEF53);
    StoreLittleEndian<std::uint32_t>(raw + 76, 1);
    StoreLittleEndian<std::uint16_t>(raw + 88, 128);
    StoreLittleEndian<std::uint32_t>(raw + 96, IncompatibleFileType);
    return bytes;
}

Superblock Decode(const std::array<std::uint8_t, SuperblockSize>& bytes)
{
    return DecodeSuperblock(bytes);
}

constexpr std::uint64_t VolumeBytes = 8192 * 1024;

} // namespace

TEST_CASE("ext2 directory entries decode with and without the filetype feature")
{
    const std::vector<std::uint8_t> block = SmallDirectory();
    const Result<RawEntry> hello = ParseEntry(block, 24, true, 2048);
    REQUIRE(hello.has_value());
    REQUIRE(hello->inode == 12);
    REQUIRE(hello->record_length == 1000);
    REQUIRE(hello->file_type == 1);
    REQUIRE(hello->name == "hello.txt");
    REQUIRE(TypeOfEntry(hello->file_type) == NodeType::File);
    REQUIRE(TypeOfEntry(7) == NodeType::SymbolicLink);
    REQUIRE(TypeOfEntry(5) == NodeType::Special);
    REQUIRE_FALSE(TypeOfEntry(0).has_value());
    REQUIRE_FALSE(TypeOfEntry(8).has_value());

    // Without the feature the type byte is the name length's upper half, which is zero here.
    std::vector<std::uint8_t> old_style = block;
    old_style[31] = 0;
    const Result<RawEntry> plain = ParseEntry(old_style, 24, false, 2048);
    REQUIRE(plain.has_value());
    REQUIRE(plain->file_type == 0);
    REQUIRE(plain->name == "hello.txt");
    // A type byte read as part of the length makes the name longer than any name may be.
    REQUIRE(ParseEntry(block, 24, false, 2048).error() == Error::Corrupt);
}

TEST_CASE("ext2 directory entries that break the format are corrupt")
{
    const auto parse = [](std::vector<std::uint8_t> block, std::size_t offset = 24)
    { return ParseEntry(block, offset, true, 2048); };
    std::vector<std::uint8_t> block = SmallDirectory();
    for (const std::uint16_t length : {0, 4, 8, 11, 13, 14, 1004, 2000, 0xFFFF})
    {
        StoreLittleEndian(block.data() + 24 + 4, length);
        INFO("record length " << length);
        REQUIRE(parse(block).error() == Error::Corrupt);
    }
    block = SmallDirectory();
    // Longer than its record of 12 bytes.
    block[12 + 6] = 5;
    REQUIRE(parse(block, 12).error() == Error::Corrupt);
    block = SmallDirectory();
    StoreLittleEndian<std::uint32_t>(block.data() + 24, 2049);
    REQUIRE(parse(block).error() == Error::Corrupt);
    block = SmallDirectory();
    block[24 + 6] = 0;
    REQUIRE(parse(block).error() == Error::Corrupt);
    // An unused record may have no name.
    StoreLittleEndian<std::uint32_t>(block.data() + 24, 0);
    REQUIRE(parse(block).has_value());
    // The header must fit in the block.
    REQUIRE(parse(SmallDirectory(), 1020).error() == Error::Corrupt);
    REQUIRE(parse(SmallDirectory(), 1024).error() == Error::Corrupt);
    REQUIRE(parse(SmallDirectory(), 5000).error() == Error::Corrupt);
}

TEST_CASE("random ext2 directory blocks parse or fail without leaving the block")
{
    std::mt19937 generator(2026);
    std::vector<std::uint8_t> block(1024);
    for (int round = 0; round < 2000; ++round)
    {
        block = SmallDirectory();
        for (int flip = 0; flip < 4; ++flip)
        {
            block[generator() % 48] ^= static_cast<std::uint8_t>(1U << (generator() % 8));
        }
        const bool has_file_type = (round % 2) == 0;
        std::size_t offset = 0;
        int entries = 0;
        while (offset < block.size())
        {
            const Result<RawEntry> entry = ParseEntry(block, offset, has_file_type, 2048);
            if (!entry)
            {
                break;
            }
            REQUIRE(entry->record_length >= 12);
            REQUIRE(offset + entry->record_length <= block.size());
            REQUIRE(entry->name.data() + entry->name.size() <=
                    reinterpret_cast<const char*>(block.data()) + offset + entry->record_length);
            offset += entry->record_length;
            REQUIRE(++entries <= 1024 / 12);
        }
    }
}

TEST_CASE("an ext2 superblock decodes and checks its geometry")
{
    std::array<std::uint8_t, SuperblockSize> bytes = SuperblockBytes();
    const Superblock superblock = Decode(bytes);
    REQUIRE(LooksLikeExt(superblock));
    REQUIRE(superblock.block_size == 1024);
    REQUIRE(superblock.group_count == 1);
    REQUIRE(superblock.inode_size == 128);
    REQUIRE(superblock.incompatible == IncompatibleFileType);
    REQUIRE(CheckSuperblock(superblock, VolumeBytes).has_value());
    REQUIRE(CheckSuperblock(superblock, VolumeBytes - 1).error() == Error::Corrupt);

    // Revision 0 has no inode size or features; whatever lies there is ignored.
    StoreLittleEndian<std::uint32_t>(bytes.data() + 76, 0);
    StoreLittleEndian<std::uint16_t>(bytes.data() + 88, 4);
    const Superblock old = Decode(bytes);
    REQUIRE(old.inode_size == 128);
    REQUIRE(old.incompatible == 0);
    REQUIRE(CheckSuperblock(old, VolumeBytes).has_value());

    StoreLittleEndian<std::uint16_t>(bytes.data() + 56, 0xEF54);
    REQUIRE_FALSE(LooksLikeExt(Decode(bytes)));
}

TEST_CASE("an ext2 superblock with an impossible layout or unknown format is refused")
{
    const auto check = [](std::size_t offset, std::uint32_t value, std::size_t width = 4)
    {
        std::array<std::uint8_t, SuperblockSize> bytes = SuperblockBytes();
        if (width == 2)
        {
            StoreLittleEndian(bytes.data() + offset, static_cast<std::uint16_t>(value));
        }
        else
        {
            StoreLittleEndian(bytes.data() + offset, value);
        }
        const Superblock superblock = DecodeSuperblock(bytes);
        return LooksLikeExt(superblock) ? CheckSuperblock(superblock, VolumeBytes)
                                        : FeatherDisk::Status(std::unexpected(Error::Corrupt));
    };
    // Block sizes above 4 KiB and revisions after 1 are formats this driver does not read.
    REQUIRE(check(24, 3).error() == Error::Unsupported);
    REQUIRE(check(76, 2).error() == Error::Unsupported);
    REQUIRE(check(24, 40).error() == Error::Corrupt);
    REQUIRE(check(0, 2049).error() == Error::Corrupt);
    REQUIRE(check(0, 0).error() == Error::Corrupt);
    REQUIRE(check(20, 0).error() == Error::Corrupt);
    REQUIRE(check(32, 8193).error() == Error::Corrupt);
    REQUIRE(check(32, 0).error() == Error::Corrupt);
    REQUIRE(check(40, 0).error() == Error::Corrupt);
    REQUIRE(check(40, 9000).error() == Error::Corrupt);
    REQUIRE(check(4, 1).error() == Error::Corrupt);
    REQUIRE(check(4, 2).error() == Error::Corrupt);
    REQUIRE(check(4, 9000).error() == Error::Corrupt);
    REQUIRE(check(88, 64, 2).error() == Error::Corrupt);
    REQUIRE(check(88, 384, 2).error() == Error::Corrupt);
    REQUIRE(check(88, 2048, 2).error() == Error::Corrupt);
    REQUIRE(check(88, 256, 2).has_value());
}

TEST_CASE("ext2 inodes decode owners, sizes, times and block numbers")
{
    std::array<std::uint8_t, DecodedInodeBytes> raw{};
    std::uint8_t* bytes = raw.data();
    StoreLittleEndian<std::uint16_t>(bytes + 0, 0100640);
    StoreLittleEndian<std::uint16_t>(bytes + 2, 0x86A0);   // 100000, low half
    StoreLittleEndian<std::uint16_t>(bytes + 120, 0x0001); // and high half
    StoreLittleEndian<std::uint16_t>(bytes + 24, 4242);
    StoreLittleEndian<std::uint32_t>(bytes + 4, 0x10);
    StoreLittleEndian<std::uint32_t>(bytes + 108, 0x2);
    StoreLittleEndian<std::uint32_t>(bytes + 8, 1600000000);
    StoreLittleEndian<std::uint32_t>(bytes + 12, 0xFFFFFFFF); // one second before 1970
    StoreLittleEndian<std::uint32_t>(bytes + 16, 1577934245);
    StoreLittleEndian<std::uint16_t>(bytes + 26, 3);
    StoreLittleEndian<std::uint32_t>(bytes + 28, 24);
    StoreLittleEndian<std::uint32_t>(bytes + 40, 777);
    StoreLittleEndian<std::uint32_t>(bytes + 40 + 14 * 4, 999);
    // i_extra_isize of 16 covers the change, modification and access time extensions.
    StoreLittleEndian<std::uint16_t>(bytes + 128, 16);
    StoreLittleEndian<std::uint32_t>(bytes + 136, (5U << 2) | 1U);
    StoreLittleEndian<std::uint32_t>(bytes + 140, 0xFFFFFFFF);

    const Inode inode = DecodeInode(raw, 0, 1024);
    REQUIRE(TypeOf(inode.mode) == NodeType::File);
    REQUIRE((inode.mode & ModePermissions) == 0640);
    REQUIRE(inode.owner == 100000);
    REQUIRE(inode.group == 4242);
    REQUIRE(inode.size == 0x200000010ULL);
    REQUIRE(inode.link_count == 3);
    REQUIRE(inode.allocated_bytes == 24 * 512);
    REQUIRE(inode.Block(0) == 777);
    REQUIRE(inode.Block(TripleIndirectBlock) == 999);
    REQUIRE(inode.change_time.seconds == -1);
    REQUIRE(inode.change_time.nanoseconds == 0);
    REQUIRE(inode.modify_time.seconds == 1577934245 + (std::int64_t{1} << 32));
    REQUIRE(inode.modify_time.nanoseconds == 5);
    REQUIRE(inode.access_time.seconds == 1600000000 + (std::int64_t{3} << 32));
    REQUIRE(inode.access_time.nanoseconds == 999999999);

    // A 128-byte inode has no extensions, and only regular files have a 64-bit size.
    const Inode small = DecodeInode(std::span(raw).first(128), 0, 1024);
    REQUIRE(small.modify_time.seconds == 1577934245);
    REQUIRE(small.modify_time.nanoseconds == 0);
    StoreLittleEndian<std::uint16_t>(bytes + 0, 040755);
    REQUIRE(DecodeInode(raw, 0, 1024).size == 0x10);
    REQUIRE(TypeOf(040755) == NodeType::Directory);
    REQUIRE(TypeOf(0120777) == NodeType::SymbolicLink);
    REQUIRE(TypeOf(0010644) == NodeType::Special);
    REQUIRE_FALSE(TypeOf(0).has_value());
    REQUIRE_FALSE(TypeOf(0170000).has_value());
}

TEST_CASE("huge_file inodes count blocks in filesystem blocks")
{
    std::array<std::uint8_t, DecodedInodeBytes> raw{};
    StoreLittleEndian<std::uint16_t>(raw.data() + 0, 0100644);
    StoreLittleEndian<std::uint32_t>(raw.data() + 28, 10);
    StoreLittleEndian<std::uint16_t>(raw.data() + 116, 1);
    REQUIRE(DecodeInode(raw, 0, 4096).allocated_bytes == 10 * 512);
    REQUIRE(DecodeInode(raw, ReadOnlyCompatibleHugeFile, 4096).allocated_bytes ==
            ((std::uint64_t{1} << 32) + 10) * 512);
    StoreLittleEndian<std::uint32_t>(raw.data() + 32, InodeHugeFile);
    REQUIRE(DecodeInode(raw, ReadOnlyCompatibleHugeFile, 4096).allocated_bytes ==
            ((std::uint64_t{1} << 32) + 10) * 4096);
}
