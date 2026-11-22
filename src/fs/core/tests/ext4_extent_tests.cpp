// SPDX-License-Identifier: MIT OR Apache-2.0
// ext4 extent trees: the node parser on its own, then files of the ext4-extents image that
// tests/make_ext2_image.sh made (holes.bin two levels deep, fragmented.bin, unwritten.bin, large/)
// read whole and in pieces, and damaged trees refused. Without e2fsprogs the image tests skip.
#include <catch2/catch_test_macros.hpp>

#include "common/little_endian.h"
#include "ext/checksum.h"
#include "ext/extent.h"
#include "fs/ext2.h"
#include "tests/ext2_image.h"
#include "tests/filesystem_helpers.h"
#include "tests/memory_device.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using FeatherDisk::Error;
using FeatherDisk::LoadLittleEndian;
using FeatherDisk::StoreLittleEndian;
using FeatherDisk::Ext::Extent;
using FeatherDisk::Ext::ExtentAt;
using FeatherDisk::Ext::ExtentHeader;
using FeatherDisk::Ext::ExtentIndex;
using FeatherDisk::Ext::ExtentIndexAt;
using FeatherDisk::Ext::ParseExtentHeader;
using FeatherDisk::Tests::ErrorOf;
using FeatherDisk::Tests::Value;

std::array<std::uint8_t, 60> RootNode(std::uint16_t entries, std::uint16_t maximum,
                                      std::uint16_t depth)
{
    std::array<std::uint8_t, 60> node{};
    StoreLittleEndian(node.data(), std::uint16_t{0xF30A});
    StoreLittleEndian(node.data() + 2, entries);
    StoreLittleEndian(node.data() + 4, maximum);
    StoreLittleEndian(node.data() + 6, depth);
    return node;
}

} // namespace

TEST_CASE("extent headers are checked before their entries are read")
{
    const std::array<std::uint8_t, 60> root = RootNode(2, 4, 1);
    const ExtentHeader header = Value(ParseExtentHeader(root));
    REQUIRE(header.entries == 2);
    REQUIRE(header.maximum == 4);
    REQUIRE(header.depth == 1);

    std::array<std::uint8_t, 60> bad_magic = root;
    bad_magic[0] ^= 1;
    REQUIRE(ErrorOf(ParseExtentHeader(bad_magic)) == Error::Corrupt);
    REQUIRE(ErrorOf(ParseExtentHeader(RootNode(5, 4, 0))) == Error::Corrupt);
    REQUIRE(ErrorOf(ParseExtentHeader(RootNode(0, 0, 0))) == Error::Corrupt);
    // Five entries would not fit in i_block's 60 bytes.
    REQUIRE(ErrorOf(ParseExtentHeader(RootNode(1, 5, 0))) == Error::Corrupt);
    REQUIRE(ErrorOf(ParseExtentHeader(std::span(root).first(8))) == Error::Corrupt);
    REQUIRE(Value(ParseExtentHeader(RootNode(0, 4, 0))).entries == 0);
}

TEST_CASE("extents decode 48-bit block numbers and unwritten lengths")
{
    std::array<std::uint8_t, 60> node = RootNode(3, 4, 0);
    // Logical 7, 32768 written blocks from 0x1234_89ABCDEF.
    StoreLittleEndian(node.data() + 12, std::uint32_t{7});
    StoreLittleEndian(node.data() + 16, std::uint16_t{32768});
    StoreLittleEndian(node.data() + 18, std::uint16_t{0x1234});
    StoreLittleEndian(node.data() + 20, std::uint32_t{0x89ABCDEF});
    // 32769: one unwritten block.
    StoreLittleEndian(node.data() + 24, std::uint32_t{40000});
    StoreLittleEndian(node.data() + 28, std::uint16_t{32769});
    StoreLittleEndian(node.data() + 32, std::uint32_t{99});
    const Extent written = ExtentAt(node, 0);
    REQUIRE(written.logical == 7);
    REQUIRE(written.length == 32768);
    REQUIRE(written.physical == 0x123489ABCDEFULL);
    REQUIRE_FALSE(written.unwritten);
    const Extent unwritten = ExtentAt(node, 1);
    REQUIRE(unwritten.length == 1);
    REQUIRE(unwritten.unwritten);
    REQUIRE(unwritten.physical == 99);

    // An index entry keeps the child's upper 16 bits after its lower 32.
    StoreLittleEndian(node.data() + 36, std::uint32_t{1000});
    StoreLittleEndian(node.data() + 40, std::uint32_t{0x55667788});
    StoreLittleEndian(node.data() + 44, std::uint16_t{0x00AB});
    const ExtentIndex index = ExtentIndexAt(node, 2);
    REQUIRE(index.logical == 1000);
    REQUIRE(index.child == 0xAB55667788ULL);
}

#ifdef FEATHERDISK_EXT_IMAGES
namespace
{

using FeatherDisk::Filesystem;
using FeatherDisk::NodeId;
using FeatherDisk::Ext::Ext2Type;
using FeatherDisk::Tests::ChangeWithDebugfs;
using FeatherDisk::Tests::DeviceFrom;
using FeatherDisk::Tests::List;
using FeatherDisk::Tests::LoadExtImage;
using FeatherDisk::Tests::MemoryDevice;
using FeatherDisk::Tests::MountedFilesystem;
using FeatherDisk::Tests::RawVolume;
using FeatherDisk::Tests::ReadAll;
using FeatherDisk::Tests::ReadAt;
using FeatherDisk::Tests::RunDebugfs;
using FeatherDisk::Tests::Sorted;
using FeatherDisk::Tests::Walk;

constexpr std::uint32_t BlockSize = 1024;
constexpr std::uint32_t InodeSeedOffset = 100;

std::string Lines(std::string_view format, int blocks, int holes_every, int hole)
{
    std::string text;
    for (int block = 0; block < blocks; ++block)
    {
        if (holes_every != 0 && block % holes_every == hole)
        {
            text.append(BlockSize, '\0');
            continue;
        }
        for (int line = 0; line < 32; ++line)
        {
            std::array<char, 64> buffer{};
            std::snprintf(buffer.data(), buffer.size(), format.data(), block, line);
            text += buffer.data();
        }
    }
    return text;
}

std::string HolesText()
{
    return Lines("block %04d line %02d with holes!!\n", 3072, 8, 5);
}

std::string FragmentedText()
{
    return Lines("piece %04d line %02d fragmented\n", 400, 0, 0);
}

std::uint32_t InodeOf(const RawVolume& raw, std::string_view name)
{
    return raw.Load32(raw.EntryOffset(2, name));
}

// The CRC32C seed of an inode's metadata blocks.
std::uint32_t InodeSeed(const std::vector<std::uint8_t>& bytes, const RawVolume& raw,
                        std::uint32_t inode)
{
    using namespace FeatherDisk::Ext;
    const Superblock superblock =
        DecodeSuperblock(std::span(bytes).subspan(1024).first<SuperblockSize>());
    return InodeChecksumSeed(VolumeChecksumSeed(superblock), inode,
                             raw.Load32(raw.InodeOffset(inode) + InodeSeedOffset));
}

// Rewrites the tail of the extent block at block after a deliberate change, so only the change
// itself is wrong.
void FixExtentTail(std::vector<std::uint8_t>& bytes, const RawVolume& raw, std::uint32_t inode,
                   std::uint64_t block)
{
    const std::span<std::uint8_t> node = std::span(bytes).subspan(block * BlockSize, BlockSize);
    const std::optional<std::size_t> tail = FeatherDisk::Ext::ExtentTailOffset(node);
    REQUIRE(tail);
    StoreLittleEndian(node.data() + *tail, FeatherDisk::Ext::ExtentBlockChecksum(
                                               node, *tail, InodeSeed(bytes, raw, inode)));
}

Error ReadError(const std::vector<std::uint8_t>& bytes, std::string_view path, std::uint64_t offset)
{
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
    MountedFilesystem ext4(Ext2Type(), *device, true);
    const NodeId file = Value(Walk(*ext4, path));
    std::array<std::uint8_t, 64> buffer{};
    return ErrorOf(ext4->Read(file, offset, buffer));
}

} // namespace
#endif

TEST_CASE("ext4 reads a file whose extent tree is two levels deep, whole and in pieces")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage("ext4-extents"));
    // The image must keep the shapes the tests are about.
    REQUIRE(RunDebugfs(*device, "extent-depth", "ex /holes.bin").output.find(" 0/ 2 ") !=
            std::string::npos);
    REQUIRE(RunDebugfs(*device, "extent-depth", "ex /fragmented.bin").output.find(" 0/ 1 ") !=
            std::string::npos);
    MountedFilesystem ext4(Ext2Type(), *device, true);
    const std::string holes = HolesText();
    const NodeId file = Value(Walk(*ext4, "holes.bin"));
    REQUIRE(Value(ext4->GetAttributes(file)).size == holes.size());
    REQUIRE(ReadAll(*ext4, file) == holes);
    // Pieces that start and end inside blocks, holes and leaves.
    std::mt19937 generator(5);
    for (int round = 0; round < 200; ++round)
    {
        const std::uint64_t offset = generator() % holes.size();
        const std::size_t length = 1 + generator() % 9000;
        INFO("offset " << offset << " length " << length);
        REQUIRE(ReadAt(*ext4, file, offset, length) == holes.substr(offset, length));
    }
    REQUIRE(ReadAt(*ext4, file, 5 * BlockSize - 3, 6) == std::string("!!\n\0\0\0", 6));
    REQUIRE(ReadAt(*ext4, file, 5 * BlockSize, BlockSize) == std::string(BlockSize, '\0'));

    const std::string fragmented = FragmentedText();
    const NodeId pieces = Value(Walk(*ext4, "fragmented.bin"));
    REQUIRE(ReadAll(*ext4, pieces) == fragmented);
    REQUIRE(ReadAt(*ext4, pieces, 1000, 50000) == fragmented.substr(1000, 50000));
#endif
}

TEST_CASE("ext4 reads unwritten extents as zeros, whatever their blocks hold")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    std::vector<std::uint8_t> bytes = LoadExtImage("ext4-extents");
    const RawVolume raw{bytes};
    const std::uint32_t inode = InodeOf(raw, "unwritten.bin");
    for (std::uint32_t logical = 1; logical < 40; ++logical)
    {
        const std::uint64_t block = raw.FileBlock(inode, logical);
        REQUIRE(block != 0);
        std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(block * BlockSize), BlockSize,
                    std::uint8_t{0xA5});
    }
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
    MountedFilesystem ext4(Ext2Type(), *device, true);
    const NodeId file = Value(Walk(*ext4, "unwritten.bin"));
    std::string expected = "start\n";
    expected.resize(40000, '\0');
    REQUIRE(ReadAll(*ext4, file) == expected);
    REQUIRE(ReadAt(*ext4, file, 3, 2000) == expected.substr(3, 2000));
#endif
}

TEST_CASE("ext4 lists and looks up a directory of 3000 entries with a two-level hash index")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage("ext4-extents"));
    REQUIRE(RunDebugfs(*device, "htree", "htree /large").output.find("Indirect levels: 1") !=
            std::string::npos);
    MountedFilesystem ext4(Ext2Type(), *device, true);
    std::vector<std::string> names;
    for (int number = 0; number < 3000; ++number)
    {
        std::array<char, 64> name{};
        std::snprintf(name.data(), name.size(), "file-with-a-long-name-for-htree-%05d.txt", number);
        names.emplace_back(name.data());
    }
    const NodeId large = Value(Walk(*ext4, "large"));
    REQUIRE(Sorted(List(*ext4, large)) == Sorted(names));
    REQUIRE(ReadAll(*ext4, Value(ext4->Lookup(large, names[2999]))) == "2999\n");
    REQUIRE(ReadAll(*ext4, Value(ext4->Lookup(large, names[0]))) == "0\n");
    REQUIRE(ErrorOf(ext4->Lookup(large, "file-with-a-long-name-for-htree-03000.txt")) ==
            Error::NotFound);
#endif
}

TEST_CASE("ext4 links read whatever the extent flag says, inline data stays refused")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage("ext4-extents"));
    REQUIRE(ChangeWithDebugfs(*device, "fast-extents", "sif /fast-link flags 0x80000").status == 0);
    REQUIRE(ChangeWithDebugfs(*device, "inline", "sif /hello.txt flags 0x10000000").status == 0);
    MountedFilesystem ext4(Ext2Type(), *device, true);
    std::array<char, 64> target{};
    const std::size_t length = Value(ext4->ReadLink(Value(Walk(*ext4, "fast-link")), target));
    REQUIRE(std::string_view(target.data(), length) == "a/b/c/deep.txt");
    std::array<std::uint8_t, 16> buffer{};
    REQUIRE(ErrorOf(ext4->Read(Value(Walk(*ext4, "hello.txt")), 0, buffer)) == Error::Unsupported);
#endif
}

TEST_CASE("damaged extent trees read as corrupt, never as other blocks")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    const std::vector<std::uint8_t> original = LoadExtImage("ext4-extents");
    std::vector<std::uint8_t> reference = original;
    const RawVolume names{reference};
    const std::uint32_t holes = InodeOf(names, "holes.bin");
    const std::vector<std::uint64_t> path = names.ExtentPath(holes, 0);
    REQUIRE(path.size() == 2);
    const std::uint64_t index_node = path[0] * BlockSize;
    const std::uint64_t leaf = path[1] * BlockSize;
    const std::uint32_t blocks = names.BlockCount();

    struct Damage
    {
        const char* what;
        std::uint64_t offset;
        std::uint32_t value;
        bool fix_tail;
        std::uint64_t node;
    };
    const std::vector<Damage> damages = {
        {"the index node's magic", index_node, 0xF30B, true, path[0]},
        {"the index node's entry count past its maximum", index_node + 2, 0x7FFF, true, path[0]},
        {"the index node's depth", index_node + 6, 3, true, path[0]},
        {"an index entry pointing back at its own node", index_node + 16,
         static_cast<std::uint32_t>(path[0]), true, path[0]},
        {"an index entry pointing past the volume", index_node + 16, blocks + 10, true, path[0]},
        {"the index node's tail checksum", index_node + 16, static_cast<std::uint32_t>(path[1] + 1),
         false, path[0]},
        {"the leaf's tail checksum", leaf + 12, 1, false, path[1]},
        {"a leaf extent past the volume", leaf + 20, blocks - 2, true, path[1]},
        {"a leaf extent at block 0", leaf + 20, 0, true, path[1]},
        {"a leaf extent of no blocks", leaf + 16, 0, true, path[1]},
        {"a leaf extent before the one ahead of it", leaf + 24, 0, true, path[1]},
    };
    for (const Damage& damage : damages)
    {
        INFO(damage.what);
        std::vector<std::uint8_t> bytes = original;
        RawVolume raw{bytes};
        if (damage.offset == index_node + 6 || damage.offset == index_node + 2 ||
            damage.offset == index_node || damage.offset == leaf + 16)
        {
            raw.Store16(damage.offset, static_cast<std::uint16_t>(damage.value));
        }
        else
        {
            raw.Store32(damage.offset, damage.value);
        }
        if (damage.fix_tail)
        {
            FixExtentTail(bytes, raw, holes, damage.node);
        }
        REQUIRE(ReadError(bytes, "holes.bin", 0) == Error::Corrupt);
    }

    // Damage to one leaf leaves the parts other leaves map readable.
    std::vector<std::uint8_t> bytes = original;
    RawVolume raw{bytes};
    raw.Store32(leaf + 20, blocks + 100);
    FixExtentTail(bytes, raw, holes, path[1]);
    {
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
        MountedFilesystem ext4(Ext2Type(), *device, true);
        const NodeId file = Value(Walk(*ext4, "holes.bin"));
        const std::uint64_t far = 3000ULL * BlockSize;
        REQUIRE(names.ExtentPath(holes, 3000) != path);
        REQUIRE(ReadAt(*ext4, file, far, 100) == HolesText().substr(far, 100));
    }

    // The root in i_block, rewritten by debugfs so that the inode's checksum still matches: a
    // wrong magic, and a depth beyond what Linux builds.
    for (const char* command :
         {"sif /holes.bin block[0] 0x0001F30B", "sif /holes.bin block[1] 0x00060004",
          "sif /holes.bin block[1] 0x00010004"})
    {
        INFO(command);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(original);
        REQUIRE(ChangeWithDebugfs(*device, "extent-root", command).status == 0);
        MountedFilesystem ext4(Ext2Type(), *device, true);
        std::array<std::uint8_t, 16> buffer{};
        REQUIRE(ErrorOf(ext4->Read(Value(Walk(*ext4, "holes.bin")), 0, buffer)) == Error::Corrupt);
    }
#endif
}
