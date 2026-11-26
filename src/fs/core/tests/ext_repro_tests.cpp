// SPDX-License-Identifier: MIT OR Apache-2.0
// Reproductions of the findings R1-01, R1-03, R1-04 and R1-05 of docs/reviews/2026-11-review.md.
// Each test asserts the correct behaviour; one tagged .repro is hidden and fails while the finding
// is open.
#include <catch2/catch_test_macros.hpp>

#include "ext/checksum.h"
#include "ext/superblock.h"
#include "fs/ext2.h"
#include "tests/crash_helpers.h"
#include "tests/ext2_image.h"
#include "tests/filesystem_helpers.h"
#include "tests/memory_device.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace FeatherDisk;
using namespace FeatherDisk::Tests;
using Ext::Ext2Type;

#ifdef FEATHERDISK_EXT_IMAGES

constexpr std::uint64_t SuperblockStart = 1024;

std::uint32_t BlockSizeOfImage(std::string_view image)
{
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(image));
    return Crash::BlockSizeOf(*device);
}

Ext::Superblock SuperblockOfBytes(std::vector<std::uint8_t>& bytes)
{
    return Ext::DecodeSuperblock(std::span(bytes).subspan(SuperblockStart).first<1024>());
}

// The extents of a file that has one block at every other logical block from 2: their number
// decides how full the leaf of its tree is. Blocks 2 to 7 are separated by blocks of another file
// so that they stay six extents, and the rest are apart by holes.
struct Fragmented
{
    NodeId file;
    std::uint64_t last_logical;
};

Fragmented MakeFragmentedFile(Filesystem& ext, std::uint32_t block_size, std::size_t extents)
{
    const NodeId file = Value(ext.Create(ext.Root(), "fragmented.bin", 0644));
    const NodeId decoy = Value(ext.Create(ext.Root(), "decoy.bin", 0644));
    std::uint64_t logical = 2;
    for (std::size_t index = 0; index < extents; ++index)
    {
        REQUIRE(WriteAt(ext, file, logical * block_size, std::string(block_size, 'F')) ==
                block_size);
        if (index < 6)
        {
            REQUIRE(WriteAt(ext, decoy, index * block_size, std::string(block_size, 'D')) ==
                    block_size);
            ++logical;
        }
        else
        {
            logical += 2;
        }
    }
    return {file, logical - (extents > 6 ? 2 : 1)};
}

#endif

} // namespace

// R1-01. A leaf that was born in the running transaction and is freed when a hole is inserted
// before its first extent is handed out again for a split of the new leaf in the same step. The
// pending forget then drops the new copy at EndStep.
TEST_CASE("R1-01 a metadata block freed and allocated again within one step keeps its contents",
          "[R1-01]")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const std::string_view image : {"ext4", "ext4-4k"})
    {
        const std::uint32_t block_size = BlockSizeOfImage(image);
        const std::size_t capacity = (block_size - 12) / 12;
        // A full leaf, and one or two short of it.
        for (const std::size_t shortfall : {std::size_t{1}, std::size_t{2}, std::size_t{3}})
        {
            INFO(image << " leaf of " << capacity - shortfall << " extents");
            const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(image));
            std::string expected;
            {
                MountedFilesystem ext(Ext2Type(), *device);
                const std::uint32_t transactions = Crash::ActivityOf(*ext).transactions;
                const Fragmented made = MakeFragmentedFile(*ext, block_size, capacity - shortfall);
                if (Crash::ActivityOf(*ext).transactions != transactions)
                {
                    FAIL("setup: the transaction committed before the step under test");
                }
                const std::uint64_t blocks = made.last_logical + 1;
                expected = Pattern(blocks * block_size);
                // One call covers the hole before the first extent and every hole between the
                // later ones: one step.
                const Result<std::size_t> written = ext->Write(made.file, 0, BytesOfText(expected));
                CHECK(written.has_value());
                if (written)
                {
                    CHECK(*written == expected.size());
                }
                std::string back(expected.size(), '\0');
                const Result<std::size_t> read =
                    ext->Read(made.file, 0,
                              std::span(reinterpret_cast<std::uint8_t*>(back.data()), back.size()));
                if (!read)
                {
                    INFO("error code " << static_cast<int>(read.error()));
                }
                if (!read)
                {
                    UNSCOPED_INFO("error " << static_cast<int>(read.error()));
                }
                CHECK(read.has_value());
                if (read)
                {
                    CHECK(back == expected);
                }
                Succeeds(ext->Sync());
            }
            MountedFilesystem again(Ext2Type(), *device, true);
            const NodeId file = Value(Walk(*again, "fragmented.bin"));
            const Attributes attributes = Value(again->GetAttributes(file));
            CHECK(attributes.size == expected.size());
            std::string back(expected.size(), '\0');
            const Result<std::size_t> read = again->Read(
                file, 0, std::span(reinterpret_cast<std::uint8_t*>(back.data()), back.size()));
            CHECK(read.has_value());
            if (read)
            {
                CHECK(back == expected);
            }
        }
    }
#endif
}

#ifdef FEATHERDISK_EXT_IMAGES
namespace
{

// The block's bit cleared in its group's bitmap, the group's free count set to 0 so that the
// allocator goes to another group and never hands the block out, and every checksum made to
// match, so that the volume mounts for writing with a block of a tree marked free.
void MarkBlockFreeInGroupThatLooksFull(std::vector<std::uint8_t>& bytes, std::uint64_t block)
{
    RawVolume raw{bytes};
    const Ext::Superblock superblock = SuperblockOfBytes(bytes);
    const std::uint64_t index = block - superblock.first_data_block;
    const auto group = static_cast<std::uint32_t>(index / superblock.blocks_per_group);
    const auto bit = static_cast<std::uint32_t>(index % superblock.blocks_per_group);
    const std::uint64_t descriptor_offset = raw.DescriptorOffset() + group * raw.DescriptorSize();
    const Ext::GroupDescriptor descriptor = Ext::DecodeGroupDescriptor(
        std::span(bytes).subspan(descriptor_offset, raw.DescriptorSize()));
    const std::uint64_t bitmap_offset =
        raw.BlockOffset(static_cast<std::uint32_t>(descriptor.block_bitmap));
    REQUIRE((bytes[bitmap_offset + bit / 8] & (1U << (bit % 8))) != 0);
    bytes[bitmap_offset + bit / 8] =
        static_cast<std::uint8_t>(bytes[bitmap_offset + bit / 8] & ~(1U << (bit % 8)));
    raw.Store16(descriptor_offset + 12, 0);
    if (raw.DescriptorSize() >= 64)
    {
        raw.Store16(descriptor_offset + 44, 0);
    }
    const std::uint32_t seed = Ext::VolumeChecksumSeed(superblock);
    const std::uint32_t sum =
        Ext::BitmapChecksum(seed, std::span<const std::uint8_t>(bytes).subspan(
                                      bitmap_offset, superblock.blocks_per_group / 8));
    raw.Store16(descriptor_offset + 24, static_cast<std::uint16_t>(sum));
    if (raw.DescriptorSize() >= 64)
    {
        raw.Store16(descriptor_offset + 56, static_cast<std::uint16_t>(sum >> 16));
    }
    raw.Store16(descriptor_offset + 30,
                Ext::GroupDescriptorChecksum(superblock, seed, group,
                                             std::span<const std::uint8_t>(bytes).subspan(
                                                 descriptor_offset, raw.DescriptorSize())));
}

bool BlockMarkedInUse(std::vector<std::uint8_t>& bytes, std::uint64_t block)
{
    RawVolume raw{bytes};
    const Ext::Superblock superblock = SuperblockOfBytes(bytes);
    const std::uint64_t index = block - superblock.first_data_block;
    const auto group = static_cast<std::uint32_t>(index / superblock.blocks_per_group);
    const auto bit = static_cast<std::uint32_t>(index % superblock.blocks_per_group);
    const Ext::GroupDescriptor descriptor = Ext::DecodeGroupDescriptor(std::span(bytes).subspan(
        raw.DescriptorOffset() + group * raw.DescriptorSize(), raw.DescriptorSize()));
    if ((descriptor.flags & Ext::GroupBlocksUnused) != 0)
    {
        return true;
    }
    return (bytes[raw.BlockOffset(static_cast<std::uint32_t>(descriptor.block_bitmap)) + bit / 8] &
            (1U << (bit % 8))) != 0;
}

// A file of one block at every other logical block from 2 on, and the blocks of the tree nodes
// below the inode on the way to its first extent, the leaf last.
struct FragmentedFile
{
    std::unique_ptr<MemoryDevice> device;
    NodeId file;
    std::uint32_t block_size;
    std::vector<std::uint64_t> nodes;
};

FragmentedFile MakeTreeFile(std::string_view image, std::size_t extents, std::size_t depth)
{
    std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(image));
    const std::uint32_t block_size = Crash::BlockSizeOf(*device);
    NodeId file = 0;
    {
        MountedFilesystem ext(Ext2Type(), *device);
        file = Value(ext->Create(ext->Root(), "r103.bin", 0644));
        for (std::size_t index = 0; index < extents; ++index)
        {
            REQUIRE(WriteAt(*ext, file, (2 + 2 * index) * block_size,
                            std::string(block_size, 'x')) == block_size);
        }
        Succeeds(ext->Sync());
    }
    std::vector<std::uint8_t> bytes = BytesOf(*device);
    RawVolume raw{bytes};
    std::vector<std::uint64_t> path = raw.ExtentPath(static_cast<std::uint32_t>(file), 2);
    if (path.size() != depth)
    {
        FAIL("setup: the tree is not " << depth << " levels below the inode");
    }
    return {std::move(device), file, block_size, std::move(path)};
}

// Every block the file maps in [0, end) is marked in use.
void EveryMappedBlockIsInUse(MemoryDevice& device, NodeId file, std::uint64_t end)
{
    std::vector<std::uint8_t> bytes = BytesOf(device);
    RawVolume raw{bytes};
    for (std::uint64_t logical = 0; logical < end; ++logical)
    {
        const std::uint64_t physical = raw.FileBlock(static_cast<std::uint32_t>(file), logical);
        if (physical != 0)
        {
            INFO("logical block " << logical << " is mapped to block " << physical);
            CHECK(BlockMarkedInUse(bytes, physical));
        }
    }
}

// The free of tree node number node (0 is the one below the inode) fails after the commit.
void FreeOfTheOldNodeFailsAfterTheCommit(std::string_view image, std::size_t extents,
                                         std::uint64_t write_from, std::size_t write_blocks,
                                         std::size_t depth = 1, std::size_t node = 0)
{
    INFO(image << ", " << extents << " extents, write of " << write_blocks << " blocks from "
               << write_from << ", node " << node << " of " << depth);
    FragmentedFile volume = MakeTreeFile(image, extents, depth);
    std::vector<std::uint8_t> bytes = BytesOf(*volume.device);
    MarkBlockFreeInGroupThatLooksFull(bytes, volume.nodes[node]);
    volume.device = DeviceFrom(bytes);
    {
        MountedFilesystem ext(Ext2Type(), *volume.device);
        static_cast<void>(
            ext->Write(volume.file, write_from * volume.block_size,
                       BytesOfText(std::string(write_blocks * volume.block_size, 'N'))));
    }
    EveryMappedBlockIsInUse(*volume.device, volume.file, 2 + 2 * extents + write_blocks);
}

} // namespace
#endif

// R1-03. The post-commit free of a replaced tree node fails (here: its bit is already clear in a
// group whose free count is 0 so that nothing else takes the block). WriteExtentFile takes the
// failure for one before the link and frees the data run the committed tree maps.
TEST_CASE("R1-03 a failure after the extent tree committed leaves no mapped block free", "[R1-03]")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    // A hole before the first extent of a one-leaf tree: the leaf is replaced.
    FreeOfTheOldNodeFailsAfterTheCommit("ext4-nojournal", 5, 0, 2);
    // A full leaf: a first hole fills in place, the second one splits it (written > 0).
    FreeOfTheOldNodeFailsAfterTheCommit("ext4-nojournal", 83, 2, 5);
    FreeOfTheOldNodeFailsAfterTheCommit("ext4-extents", 83, 2, 5);
#endif
}

// A tree two levels deep: the commit is at the inode's root, with an index node and a leaf
// replaced, and either free fails (the index node's is the first, the leaf's would be next).
TEST_CASE("a failed free of a node of a deeper extent tree after its commit leaves no mapped block "
          "free",
          "[R1-03]")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const std::string_view image : {"ext4-nojournal", "ext4-extents"})
    {
        for (const std::size_t node : {std::size_t{0}, std::size_t{1}})
        {
            FreeOfTheOldNodeFailsAfterTheCommit(image, 340, 0, 2, 2, node);
        }
    }
#endif
}

// The volume has room for the data of a write but not for the tree node that replaces the leaf: the
// failure is before the commit, so the data run goes back and the volume is whole.
TEST_CASE("a write that finds no room for a new tree node gives its data blocks back", "[R1-03]")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const std::string_view image : {"ext4-nojournal", "ext4-extents"})
    {
        INFO(image);
        FragmentedFile volume = MakeTreeFile(image, 5, 1);
        {
            MountedFilesystem ext(Ext2Type(), *volume.device);
            const NodeId filler = Value(ext->Create(ext->Root(), "filler.bin", 0644));
            const std::string block(volume.block_size, 'f');
            std::uint64_t offset = 0;
            while (FreeBlocks(*ext) > 0 && WriteAt(*ext, filler, offset, block) == block.size())
            {
                offset += block.size();
            }
            Succeeds(ext->Sync());
            // Two blocks for the data of the write, none for the leaf that replaces the old one.
            while (FreeBlocks(*ext) < 2)
            {
                offset -= block.size();
                Succeeds(ext->Truncate(filler, offset));
            }
            REQUIRE(FreeBlocks(*ext) == 2);
            Succeeds(ext->Sync());
            const Result<std::size_t> written =
                ext->Write(volume.file, 0, BytesOfText(std::string(2 * volume.block_size, 'N')));
            CHECK(!written);
            CHECK(FreeBlocks(*ext) == 2);
        }
        std::vector<std::uint8_t> bytes = BytesOf(*volume.device);
        const RawVolume raw{bytes};
        CHECK(raw.FileBlock(static_cast<std::uint32_t>(volume.file), 0) == 0);
        CHECK(raw.FileBlock(static_cast<std::uint32_t>(volume.file), 1) == 0);
        CheckClean(*volume.device, "r1-03-no-room-for-a-node");
    }
#endif
}

// R1-04. A truncate does not touch the tail of the new last block before its new size is durable,
// so a volume that stops anywhere before that has the old size and the old bytes.
TEST_CASE("R1-04 a truncate that did not commit leaves the old size and the old bytes", "[R1-04]")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const std::string_view image : {"ext4", "ext3", "ext4-blockmap", "ext4-nojournal", "1k"})
    {
        INFO(image);
        const bool journalled = image != "ext4-nojournal" && image != "1k";
        std::uint64_t writes = 0;
        // The first pass counts the writes of the truncate, the next ones stop after each of them.
        for (std::uint64_t cut = 0; cut <= writes; ++cut)
        {
            INFO("stopped after " << cut << " of " << writes << " writes");
            const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(image));
            const std::uint32_t block_size = Crash::BlockSizeOf(*device);
            const std::string original = Pattern(3 * block_size - 72);
            const std::size_t shorter = block_size + block_size / 2;
            std::unique_ptr<MemoryDevice> stopped;
            {
                MountedFilesystem ext(Ext2Type(), *device);
                const NodeId file = WriteNewFile(*ext, "truncated.bin", original);
                Succeeds(ext->Sync());
                const std::uint64_t start = device->write_count;
                if (cut > 0 || writes != 0)
                {
                    device->FailWritesAfter(start + cut);
                }
                static_cast<void>(ext->Truncate(file, shorter));
                if (writes == 0)
                {
                    writes = device->write_count - start;
                }
                stopped = CopyOf(*device);
                device->FailWritesAfter(UINT64_MAX);
            }
            if (journalled)
            {
                ReplayByMounting(*stopped);
            }
            MountedFilesystem after(Ext2Type(), *stopped, true);
            const std::string contents = ReadAll(*after, Value(Walk(*after, "truncated.bin")));
            if (journalled)
            {
                // Either the truncate did not happen or it did, never half of it.
                const bool untouched = contents == original;
                const bool done = contents == original.substr(0, shorter);
                CHECK((untouched || done));
            }
            else
            {
                // Without a journal the blocks go before the size, so the file may be shorter or
                // have holes; only the tail of its last block must still be the old one.
                const std::size_t end = std::min<std::size_t>(contents.size(), 2 * block_size);
                if (contents.size() == original.size())
                {
                    CHECK(contents.substr(shorter, end - shorter) ==
                          original.substr(shorter, end - shorter));
                }
            }
        }
        CHECK(writes > 0);
    }
#endif
}

namespace
{

#ifdef FEATHERDISK_EXT_IMAGES

std::string OfFile(MemoryDevice& device, std::string_view name)
{
    MountedFilesystem ext(Ext2Type(), device, true);
    return ReadAll(*ext, Value(Walk(*ext, name)));
}

// The truncate fails after the tree walk has started and before anything is stored: the file keeps
// its size and every byte, also after the volume is mounted again.
void FailedTruncateKeepsTheBytes(std::string_view image, std::size_t extents, bool block_map)
{
    INFO(image << (block_map ? ", block map" : ", extent tree"));
    std::unique_ptr<MemoryDevice> device;
    std::uint64_t unreadable = 0;
    std::uint32_t block_size = 0;
    std::string original;
    std::size_t shorter = 0;
    NodeId file = 0;
    if (block_map)
    {
        device = DeviceFrom(LoadExtImage(image));
        block_size = Crash::BlockSizeOf(*device);
        original = Pattern(20 * block_size - 72);
        shorter = 5 * block_size + block_size / 2;
        {
            MountedFilesystem ext(Ext2Type(), *device);
            file = WriteNewFile(*ext, "truncated.bin", original);
            Succeeds(ext->Sync());
        }
        std::vector<std::uint8_t> bytes = BytesOf(*device);
        RawVolume raw{bytes};
        // The single indirect block: wholly past the new end, so it is read to be freed.
        unreadable = raw.InodeBlock(static_cast<std::uint32_t>(file), 12);
    }
    else
    {
        FragmentedFile volume = MakeTreeFile(image, extents, 2);
        device = std::move(volume.device);
        block_size = volume.block_size;
        file = volume.file;
        // Logical block 2 is mapped and full; the leaf of the last extent is read first.
        shorter = 2 * block_size + block_size / 2;
        std::vector<std::uint8_t> bytes = BytesOf(*device);
        RawVolume raw{bytes};
        const std::uint64_t last = 8 + 2 * (extents - 7);
        const std::vector<std::uint64_t> path =
            raw.ExtentPath(static_cast<std::uint32_t>(file), static_cast<std::uint32_t>(last));
        REQUIRE(path.size() == 2);
        REQUIRE(path.back() != volume.nodes.back());
        unreadable = path.back();
        MountedFilesystem ext(Ext2Type(), *device, true);
        original = ReadAll(*ext, file);
    }
    REQUIRE(original.size() > shorter);
    {
        MountedFilesystem ext(Ext2Type(), *device);
        device->FailReadsAt(unreadable * block_size / ImageSectorSize);
        CHECK(!ext->Truncate(file, shorter));
        device->RestoreReads();
    }
    ReplayByMounting(*device);
    CHECK(OfFile(*device, block_map ? "truncated.bin" : "r103.bin") == original);
}

#endif

} // namespace

// R1-04, the other way to a truncate that does not happen: it fails after the point where the tail
// used to be zeroed.
TEST_CASE("R1-04 a truncate that fails leaves the file at its old size with its old bytes",
          "[R1-04]")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    FailedTruncateKeepsTheBytes("ext3", 0, true);
    FailedTruncateKeepsTheBytes("ext4-blockmap", 0, true);
    FailedTruncateKeepsTheBytes("ext4-extents", 340, false);
    FailedTruncateKeepsTheBytes("ext4-nojournal", 340, false);
#endif
}

// What a truncate leaves past the new end of its last block reads as zeros once the file grows over
// it again, by a truncate or a write, in the same mount or after a mount of the volume.
TEST_CASE("R1-04 the old tail of a truncated file reads as zeros when the file grows again",
          "[R1-04]")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const std::string_view image :
         {"1k", "4k", "ext3", "ext4", "ext4-blockmap", "ext4-nojournal", "ext4-4k"})
    {
        for (const bool remount : {false, true})
        {
            for (const bool by_write : {false, true})
            {
                INFO(image << (remount ? ", remounted" : "") << (by_write ? ", by a write" : ""));
                const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(image));
                const std::uint32_t block_size = Crash::BlockSizeOf(*device);
                const std::string original = Pattern(3 * block_size - 72);
                const std::size_t shorter = block_size + block_size / 2;
                const std::size_t grown = 4 * block_size;
                std::string expected = original.substr(0, shorter);
                const auto grow = [&](Filesystem& ext, NodeId file)
                {
                    if (by_write)
                    {
                        const std::string tail = "end";
                        expected.resize(grown - tail.size(), '\0');
                        expected += tail;
                        REQUIRE(WriteAt(ext, file, grown - tail.size(), tail) == tail.size());
                    }
                    else
                    {
                        expected.resize(grown, '\0');
                        Succeeds(ext.Truncate(file, grown));
                    }
                    CHECK(ReadAll(ext, file) == expected);
                };
                {
                    MountedFilesystem ext(Ext2Type(), *device);
                    NodeId file = WriteNewFile(*ext, "truncated.bin", original);
                    Succeeds(ext->Truncate(file, shorter));
                    if (!remount)
                    {
                        grow(*ext, file);
                    }
                }
                if (remount)
                {
                    MountedFilesystem ext(Ext2Type(), *device);
                    grow(*ext, Value(Walk(*ext, "truncated.bin")));
                }
                CheckClean(*device, "r1-04-grow");
                CHECK(OfFile(*device, "truncated.bin") == expected);
            }
        }
    }
#endif
}

// Linux grows a file over the block it reads from the disk and does not zero the old tail, so what
// follows the new end of the last block must be zero on the device once the truncate is durable.
TEST_CASE("R1-04 a truncate leaves the rest of its last block zero on the device", "[R1-04]")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const std::string_view image :
         {"1k", "4k", "ext3", "ext4", "ext4-blockmap", "ext4-nojournal", "ext4-4k"})
    {
        INFO(image);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(image));
        const std::uint32_t block_size = Crash::BlockSizeOf(*device);
        const std::string original = Pattern(3 * block_size - 72);
        const std::size_t shorter = block_size + block_size / 2;
        NodeId file = 0;
        {
            MountedFilesystem ext(Ext2Type(), *device);
            file = WriteNewFile(*ext, "truncated.bin", original);
            Succeeds(ext->Sync());
            Succeeds(ext->Truncate(file, shorter));
            // Sync writes no file data, so the zeroing is Truncate's.
            Succeeds(ext->Sync());
            const std::unique_ptr<MemoryDevice> stopped = CopyOf(*device);
            if (image != "1k" && image != "4k" && image != "ext4-nojournal")
            {
                // The inode's home block is written at a checkpoint.
                ReplayByMounting(*stopped);
            }
            std::vector<std::uint8_t> bytes = BytesOf(*stopped);
            RawVolume raw{bytes};
            const std::uint64_t block = raw.FileBlock(static_cast<std::uint32_t>(file), 1);
            REQUIRE(block != 0);
            const std::uint64_t at = raw.BlockOffset(static_cast<std::uint32_t>(block));
            CHECK(bytes[at + shorter - block_size - 1] ==
                  static_cast<std::uint8_t>(original[shorter - 1]));
            for (std::size_t index = shorter - block_size; index < block_size; ++index)
            {
                REQUIRE(bytes[at + index] == 0);
            }
        }
        CheckClean(*device, "r1-04-raw-tail");
    }
#endif
}

#ifdef FEATHERDISK_EXT_IMAGES
namespace
{

// A directory's link count set in the bytes of a volume, its inode checksum made to match.
void SetLinkCount(std::vector<std::uint8_t>& bytes, NodeId node, std::uint16_t count)
{
    RawVolume raw{bytes};
    const auto inode = static_cast<std::uint32_t>(node);
    raw.Store16(raw.InodeOffset(inode) + 26, count);
    const Ext::Superblock superblock = SuperblockOfBytes(bytes);
    if (Ext::HasMetadataChecksums(superblock))
    {
        constexpr std::uint32_t GenerationOffset = 100;
        Ext::StoreInodeChecksum(
            std::span(bytes).subspan(raw.InodeOffset(inode), raw.InodeSize()),
            Ext::InodeChecksumSeed(Ext::VolumeChecksumSeed(superblock), inode,
                                   raw.Load32(raw.InodeOffset(inode) + GenerationOffset)));
    }
}

} // namespace
#endif

// R1-05. Renaming a directory onto an empty directory of a parent whose link count is at the
// limit is legal: the replaced directory's ".." link is replaced by the new one.
TEST_CASE("R1-05 a directory renamed onto an empty one in a parent at the link limit", "[R1-05]")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    struct Case
    {
        std::string_view image;
        std::uint16_t limit;
        bool same_parent;
    };
    // ext2 and ext3 stop at 32000; ext4's dir_nlink counts up to 65000 and then 1.
    for (const Case& which :
         {Case{"1k", 32000, false}, Case{"1k", 32000, true}, Case{"ext3", 32000, false},
          Case{"ext4", 65000, false}, Case{"ext4", 65000, true}})
    {
        INFO(which.image << (which.same_parent ? " same parent" : " other parent"));
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(which.image));
        NodeId parent = 0;
        {
            MountedFilesystem ext(Ext2Type(), *device);
            parent = Value(ext->MakeDirectory(ext->Root(), "full-parent", 0755));
            static_cast<void>(Value(ext->MakeDirectory(parent, "target", 0755)));
            static_cast<void>(Value(
                ext->MakeDirectory(which.same_parent ? parent : ext->Root(), "source", 0755)));
            static_cast<void>(Value(ext->MakeDirectory(ext->Root(), "mover", 0755)));
            Succeeds(ext->Sync());
        }
        std::vector<std::uint8_t> bytes = BytesOf(*device);
        SetLinkCount(bytes, parent, which.limit);
        const std::unique_ptr<MemoryDevice> limited = DeviceFrom(bytes);
        MountedFilesystem ext(Ext2Type(), *limited);
        const NodeId from = which.same_parent ? parent : ext->Root();
        const NodeId source =
            Value(Walk(*ext, which.same_parent ? "full-parent/source" : "source"));
        const std::uint32_t from_links = Value(ext->GetAttributes(from)).link_count;
        const Status renamed = ext->Rename(from, "source", parent, "target");
        if (!renamed)
        {
            UNSCOPED_INFO("error " << static_cast<int>(renamed.error()));
        }
        REQUIRE(renamed.has_value());
        // The replaced directory's link left as the moved one's arrived, so the parent that was
        // the target's own loses one only when the source stayed in it.
        CHECK(Value(ext->GetAttributes(parent)).link_count ==
              (which.same_parent ? which.limit - 1 : which.limit));
        if (!which.same_parent)
        {
            CHECK(Value(ext->GetAttributes(from)).link_count == from_links - 1);
        }
        CHECK(Value(Walk(*ext, "full-parent/target")) == source);
        CHECK(Value(ext->Lookup(source, "..")) == parent);
        // The volume is still writable, in no error state.
        CHECK(ext->Create(ext->Root(), "after-the-rename", 0644).has_value());
        if (!which.same_parent && which.limit == 32000)
        {
            // A name that is not a replacement does take a link, and finds none to take.
            const Status refused = ext->Rename(ext->Root(), "mover", parent, "one-more");
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error() == Error::TooManyLinks);
            CHECK(ext->Create(ext->Root(), "after-the-refusal", 0644).has_value());
        }
        Succeeds(ext->Sync());
    }
#endif
}
