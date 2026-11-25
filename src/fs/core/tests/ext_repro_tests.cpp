// SPDX-License-Identifier: MIT OR Apache-2.0
// Reproductions of the findings R1-01 to R1-05 of docs/reviews/2026-11-review.md. Hidden: each test
// asserts the correct behaviour and fails while the finding is open.
#include <catch2/catch_test_macros.hpp>

#include "ext/checksum.h"
#include "ext/superblock.h"
#include "ext/transaction.h"
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
using Ext::BlockLayer;
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
          "[.repro][R1-01]")
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

// R1-02. EndStep reserves running.Count() + step.Count() in the running layer, which counts the
// blocks of both layers twice. A volume with a 1024-copy layer and a step that touches 100 blocks
// the running transaction already holds fails with OutOfMemory although the distinct blocks fit.
// The step that touches hundreds of metadata blocks cannot be made on the test images (the
// layers hold 4 MiB: 4096 copies of 1 KiB, and the 4 KiB image's journal is the smaller limit
// only for the distinct count), so this checks the arithmetic EndStep performs on the real layer.
TEST_CASE("R1-02 EndStep's reservation does not count a block of both layers twice",
          "[.repro][R1-02]")
{
    constexpr std::uint32_t Size = 4096;
    BlockLayer running(Size);
    BlockLayer step(Size);
    for (std::uint64_t block = 0; block < 400; ++block)
    {
        REQUIRE(running.Hold(block) != nullptr);
    }
    for (std::uint64_t block = 300; block < 930; ++block)
    {
        REQUIRE(step.Hold(block) != nullptr);
    }
    // 930 distinct blocks, below the 1024 a layer may hold.
    REQUIRE(running.Maximum() == 1024);
    // What EndStep asks of the running layer before it absorbs the step.
    CHECK(running.Reserve(running.Count() + step.Count()));
    // What the absorption itself needs, and gets.
    Succeeds(running.Absorb(step));
    CHECK(running.Count() == 930);
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

void FreeOfTheOldNodeFailsAfterTheCommit(std::string_view image, std::size_t extents,
                                         std::uint64_t write_from, std::size_t write_blocks)
{
    INFO(image << ", " << extents << " extents, write of " << write_blocks << " blocks from "
               << write_from);
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
    {
        RawVolume raw{bytes};
        const std::vector<std::uint64_t> path = raw.ExtentPath(static_cast<std::uint32_t>(file), 2);
        if (path.size() != 1)
        {
            FAIL("setup: the tree is not one leaf below the inode");
        }
        MarkBlockFreeInGroupThatLooksFull(bytes, path[0]);
    }
    device = DeviceFrom(bytes);
    {
        MountedFilesystem ext(Ext2Type(), *device);
        static_cast<void>(ext->Write(file, write_from * block_size,
                                     BytesOfText(std::string(write_blocks * block_size, 'N'))));
    }
    bytes = BytesOf(*device);
    RawVolume raw{bytes};
    for (std::uint64_t logical = 0; logical < 2 + 2 * extents + write_blocks; ++logical)
    {
        const std::uint64_t physical = raw.FileBlock(static_cast<std::uint32_t>(file), logical);
        if (physical != 0)
        {
            INFO("logical block " << logical << " is mapped to block " << physical);
            CHECK(BlockMarkedInUse(bytes, physical));
        }
    }
}

} // namespace
#endif

// R1-03. The post-commit free of a replaced tree node fails (here: its bit is already clear in a
// group whose free count is 0 so that nothing else takes the block). WriteExtentFile takes the
// failure for one before the link and frees the data run the committed tree maps.
TEST_CASE("R1-03 a failure after the extent tree committed leaves no mapped block free",
          "[.repro][R1-03]")
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

// R1-04. Truncate zeroes the tail of the new last block on the device before the new size is
// committed, so a volume that stops before the commit has the old size and a zeroed tail.
TEST_CASE("R1-04 a truncate that did not commit leaves the old size and the old bytes",
          "[.repro][R1-04]")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const std::string_view image : {"ext4", "ext3"})
    {
        INFO(image);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(image));
        const std::uint32_t block_size = Crash::BlockSizeOf(*device);
        const std::string original = Pattern(3 * block_size - 72);
        const std::size_t shorter = block_size + block_size / 2;
        std::unique_ptr<MemoryDevice> stopped;
        {
            MountedFilesystem ext(Ext2Type(), *device);
            const NodeId file = WriteNewFile(*ext, "truncated.bin", original);
            Succeeds(ext->Sync());
            const std::uint32_t transactions = Crash::ActivityOf(*ext).transactions;
            Succeeds(ext->Truncate(file, shorter));
            if (Crash::ActivityOf(*ext).transactions != transactions)
            {
                FAIL("setup: the truncate committed");
            }
            stopped = CopyOf(*device);
        }
        ReplayByMounting(*stopped);
        MountedFilesystem after(Ext2Type(), *stopped, true);
        const NodeId file = Value(Walk(*after, "truncated.bin"));
        const std::string contents = ReadAll(*after, file);
        // Either the truncate did not happen or it did, never half of it.
        const bool untouched = contents == original;
        const bool done = contents == original.substr(0, shorter);
        CHECK((untouched || done));
    }
#endif
}

// R1-05. Renaming a directory onto an empty directory of a parent whose link count is at the
// limit is legal: the replaced directory's ".." link is replaced by the new one.
TEST_CASE("R1-05 a directory renamed onto an empty one in a parent at the link limit",
          "[.repro][R1-05]")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const std::string_view image : {"1k", "ext3"})
    {
        INFO(image);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(image));
        NodeId parent = 0;
        {
            MountedFilesystem ext(Ext2Type(), *device);
            parent = Value(ext->MakeDirectory(ext->Root(), "full-parent", 0755));
            static_cast<void>(Value(ext->MakeDirectory(parent, "target", 0755)));
            static_cast<void>(Value(ext->MakeDirectory(ext->Root(), "source", 0755)));
            Succeeds(ext->Sync());
        }
        std::vector<std::uint8_t> bytes = BytesOf(*device);
        RawVolume raw{bytes};
        raw.Store16(raw.InodeOffset(static_cast<std::uint32_t>(parent)) + 26, 32000);
        const std::unique_ptr<MemoryDevice> limited = DeviceFrom(bytes);
        MountedFilesystem ext(Ext2Type(), *limited);
        const NodeId source = Value(Walk(*ext, "source"));
        const Status renamed = ext->Rename(ext->Root(), "source", parent, "target");
        if (!renamed)
        {
            UNSCOPED_INFO("error " << static_cast<int>(renamed.error()));
        }
        CHECK(renamed.has_value());
        // The volume must still be writable and the parent's count unchanged.
        CHECK(ext->Create(ext->Root(), "after-the-rename", 0644).has_value());
        CHECK(Value(ext->GetAttributes(parent)).link_count == 32000);
        if (renamed)
        {
            CHECK(Value(Walk(*ext, "full-parent/target")) == source);
        }
    }
#endif
}
