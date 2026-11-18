// SPDX-License-Identifier: MIT OR Apache-2.0
// Writes through ext4 extent trees on volumes without a journal (tests/make_ext2_image.sh's
// ext4-nojournal, mke2fs's default ext4 with 1 KiB blocks, and ext4-mixed, whose image files keep
// block maps): new nodes, trees that grow two levels deep, holes, unwritten extents, truncation,
// removal, large directories and a crash at every write. e2fsck -fn checks each volume, and
// debugfs reads back the trees and the contents.
#include <catch2/catch_test_macros.hpp>

#include "fs/ext2.h"
#include "tests/ext2_image.h"
#include "tests/filesystem_helpers.h"
#include "tests/memory_device.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef FEATHERDISK_EXT_IMAGES
namespace
{

using FeatherDisk::Error;
using FeatherDisk::Filesystem;
using FeatherDisk::NodeId;
using FeatherDisk::Result;
using FeatherDisk::Ext::Ext2Type;
using FeatherDisk::Tests::BytesOf;
using FeatherDisk::Tests::BytesOfText;
using FeatherDisk::Tests::ChangeWithDebugfs;
using FeatherDisk::Tests::CheckClean;
using FeatherDisk::Tests::CopyOf;
using FeatherDisk::Tests::DeviceFrom;
using FeatherDisk::Tests::ErrorOf;
using FeatherDisk::Tests::FreeBlocks;
using FeatherDisk::Tests::HaveE2fsck;
using FeatherDisk::Tests::List;
using FeatherDisk::Tests::LoadExtImage;
using FeatherDisk::Tests::MemoryDevice;
using FeatherDisk::Tests::MountedFilesystem;
using FeatherDisk::Tests::NumberedName;
using FeatherDisk::Tests::OnlyRoutineRepairs;
using FeatherDisk::Tests::Pattern;
using FeatherDisk::Tests::RawVolume;
using FeatherDisk::Tests::ReadAll;
using FeatherDisk::Tests::ReadAt;
using FeatherDisk::Tests::RunDebugfs;
using FeatherDisk::Tests::RunE2fsck;
using FeatherDisk::Tests::ScriptWithDebugfs;
using FeatherDisk::Tests::Succeeds;
using FeatherDisk::Tests::ToolRun;
using FeatherDisk::Tests::Value;
using FeatherDisk::Tests::Walk;
using FeatherDisk::Tests::WriteAt;

constexpr std::uint32_t ExtentsFlag = 0x80000;
constexpr std::uint32_t BlockSize = 1024;

// A line of debugfs's "ex": an index entry or, in a leaf, an extent.
struct DumpedEntry
{
    unsigned level;
    unsigned depth;
    std::uint64_t logical;
    std::uint64_t length;
    std::uint64_t physical;
    bool unwritten;
};

std::vector<DumpedEntry> DumpExtents(MemoryDevice& device, std::string_view path)
{
    const ToolRun run = RunDebugfs(device, "extent-dump", "ex " + std::string(path));
    REQUIRE(run.ran);
    std::vector<DumpedEntry> entries;
    std::istringstream lines(run.output);
    std::string line;
    while (std::getline(lines, line))
    {
        std::replace(line.begin(), line.end(), '/', ' ');
        std::replace(line.begin(), line.end(), '-', ' ');
        std::istringstream fields(line);
        DumpedEntry entry{};
        unsigned position = 0;
        unsigned count = 0;
        std::uint64_t end = 0;
        if (!(fields >> entry.level >> entry.depth >> position >> count >> entry.logical >> end >>
              entry.physical))
        {
            continue;
        }
        if (entry.level == entry.depth)
        {
            std::uint64_t last = 0;
            std::string flag;
            fields >> last >> entry.length >> flag;
            entry.unwritten = flag == "Uninit";
        }
        else
        {
            fields >> entry.length;
        }
        entries.push_back(entry);
    }
    return entries;
}

unsigned TreeDepth(MemoryDevice& device, std::string_view path)
{
    const std::vector<DumpedEntry> entries = DumpExtents(device, path);
    return entries.empty() ? 0 : entries.front().depth;
}

std::vector<DumpedEntry> Leaves(MemoryDevice& device, std::string_view path)
{
    std::vector<DumpedEntry> leaves;
    for (const DumpedEntry& entry : DumpExtents(device, path))
    {
        if (entry.level == entry.depth)
        {
            leaves.push_back(entry);
        }
    }
    return leaves;
}

// The tree's blocks below the root: one per index entry.
std::uint64_t TreeBlocks(MemoryDevice& device, std::string_view path)
{
    const std::vector<DumpedEntry> entries = DumpExtents(device, path);
    return static_cast<std::uint64_t>(std::count_if(entries.begin(), entries.end(),
                                                    [](const DumpedEntry& entry)
                                                    { return entry.level != entry.depth; }));
}

std::uint32_t InodeFlags(MemoryDevice& device, NodeId node)
{
    std::vector<std::uint8_t> bytes = BytesOf(device);
    const RawVolume raw{bytes};
    return raw.Load32(raw.InodeOffset(static_cast<std::uint32_t>(node)) + 32);
}

// The extent root's header in i_block: entries, maximum and depth.
std::array<std::uint16_t, 3> RootHeader(MemoryDevice& device, NodeId node)
{
    std::vector<std::uint8_t> bytes = BytesOf(device);
    const RawVolume raw{bytes};
    const std::uint64_t root = raw.InodeOffset(static_cast<std::uint32_t>(node)) + 40;
    REQUIRE(raw.Load16(root) == 0xF30A);
    return {raw.Load16(root + 2), raw.Load16(root + 4), raw.Load16(root + 6)};
}

void CheckDebugfsCat(MemoryDevice& device, std::string_view path, const std::string& expected)
{
    const ToolRun run = RunDebugfs(device, "extent-cat", "cat " + std::string(path));
    if (run.ran)
    {
        REQUIRE(run.output == expected);
    }
}

void Overwrite(std::string& model, std::uint64_t offset, const std::string& data)
{
    if (model.size() < offset + data.size())
    {
        model.resize(static_cast<std::size_t>(offset + data.size()), '\0');
    }
    model.replace(static_cast<std::size_t>(offset), data.size(), data);
}

std::string Block(char fill, std::size_t length = BlockSize)
{
    return std::string(length, fill);
}

// Two files appended a block at a time in turn take every other block, so each gets an extent
// per block: count of them grows a tree two levels deep with 1 KiB blocks (more than 4 * 84).
void Interleave(Filesystem& ext4, NodeId first, NodeId second, int count, std::string& first_text,
                std::string& second_text)
{
    for (int index = 0; index < count; ++index)
    {
        const std::string one = NumberedName("first", index, BlockSize);
        const std::string two = NumberedName("second", index, BlockSize);
        REQUIRE(WriteAt(ext4, first, first_text.size(), one) == one.size());
        REQUIRE(WriteAt(ext4, second, second_text.size(), two) == two.size());
        first_text += one;
        second_text += two;
    }
}

} // namespace
#endif

TEST_CASE("ext4 with extents and no journal mounts writable and gives new nodes extents")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const std::string_view variant : {"ext4-nojournal", "ext4-mixed"})
    {
        INFO(variant);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(variant));
        NodeId file = 0;
        NodeId directory = 0;
        NodeId slow = 0;
        NodeId fast = 0;
        const std::string target = Pattern(100);
        const std::string text = Pattern(5000);
        {
            MountedFilesystem ext4(Ext2Type(), *device);
            REQUIRE(ext4->GetCapabilities().writable);
            REQUIRE(ext4->GetCapabilities().maximum_file_size ==
                    ((std::uint64_t{1} << 32) - 1) * Value(ext4->Statistics()).block_size);
            const NodeId root = ext4->Root();
            file = Value(ext4->Create(root, "new.txt", 0644));
            directory = Value(ext4->MakeDirectory(root, "new-directory", 0755));
            slow = Value(ext4->SymbolicLink(root, "slow", target));
            fast = Value(ext4->SymbolicLink(root, "fast", "short"));
        }
        REQUIRE((InodeFlags(*device, file) & ExtentsFlag) != 0);
        REQUIRE(RootHeader(*device, file) == std::array<std::uint16_t, 3>{0, 4, 0});
        REQUIRE((InodeFlags(*device, directory) & ExtentsFlag) != 0);
        REQUIRE(RootHeader(*device, directory) == std::array<std::uint16_t, 3>{1, 4, 0});
        REQUIRE((InodeFlags(*device, slow) & ExtentsFlag) != 0);
        REQUIRE((InodeFlags(*device, fast) & ExtentsFlag) == 0);
        CheckClean(*device, std::string(variant) + "-new-nodes");
        {
            MountedFilesystem ext4(Ext2Type(), *device);
            REQUIRE(WriteAt(*ext4, file, 0, text) == text.size());
            REQUIRE(ReadAll(*ext4, file) == text);
            // The image's own files: extents on ext4-nojournal, block maps on ext4-mixed.
            const NodeId big = Value(Walk(*ext4, "big.txt"));
            std::string big_text = ReadAll(*ext4, big);
            const std::string tail = Pattern(9000);
            REQUIRE(WriteAt(*ext4, big, big_text.size() - 100, tail) == tail.size());
            Overwrite(big_text, big_text.size() - 100, tail);
            REQUIRE(WriteAt(*ext4, big, 1000, "middle") == 6);
            Overwrite(big_text, 1000, "middle");
            Succeeds(ext4->Truncate(big, 300000));
            big_text.resize(300000);
            REQUIRE(ReadAll(*ext4, big) == big_text);
            for (int number = 0; number < 40; ++number)
            {
                REQUIRE(Value(ext4->Create(Value(Walk(*ext4, "new-directory")),
                                           NumberedName("entry", number, 200), 0644)) != 0);
            }
            REQUIRE(List(*ext4, Value(Walk(*ext4, "new-directory"))).size() == 40);
            const bool mixed = variant == "ext4-mixed";
            REQUIRE(((InodeFlags(*device, big) & ExtentsFlag) == 0) == mixed);
            const ToolRun link = RunDebugfs(*device, "extent-link", "cat /slow");
            if (link.ran)
            {
                REQUIRE(link.output == target);
            }
        }
        CheckClean(*device, std::string(variant) + "-new-nodes-written");
        CheckDebugfsCat(*device, "/new.txt", text);
        REQUIRE(Leaves(*device, "/new-directory").size() == 1);
    }
#endif
}

TEST_CASE("ext4 extent files grow from one extent to trees two levels deep")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage("ext4-nojournal"));
    const std::string sequential = Pattern(3 * 1024 * 1024 + 123);
    std::string first_text;
    std::string second_text;
    std::uint32_t free = 0;
    std::uint32_t after_sequential = 0;
    {
        MountedFilesystem ext4(Ext2Type(), *device);
        free = FreeBlocks(*ext4);
        const NodeId root = ext4->Root();
        // Unaligned pieces of a long file, each run of free blocks one extent: the image's free
        // space starts with single blocks between files.
        const NodeId large = Value(ext4->Create(root, "sequential.bin", 0644));
        for (std::size_t offset = 0; offset < sequential.size(); offset += 7001)
        {
            const std::string piece = sequential.substr(offset, 7001);
            REQUIRE(WriteAt(*ext4, large, offset, piece) == piece.size());
        }
        REQUIRE(ReadAll(*ext4, large) == sequential);
        after_sequential = FreeBlocks(*ext4);

        const NodeId first = Value(ext4->Create(root, "first.bin", 0644));
        const NodeId second = Value(ext4->Create(root, "second.bin", 0644));
        // Past the root's four extents (one level), past a leaf's 84 (two leaves), and past four
        // leaves (two levels).
        Interleave(*ext4, first, second, 3, first_text, second_text);
        REQUIRE(RootHeader(*device, first)[2] == 0);
        Interleave(*ext4, first, second, 3, first_text, second_text);
        REQUIRE(RootHeader(*device, first)[2] == 1);
        Interleave(*ext4, first, second, 100, first_text, second_text);
        REQUIRE(RootHeader(*device, first)[2] == 1);
        Interleave(*ext4, first, second, 300, first_text, second_text);
        REQUIRE(RootHeader(*device, first)[2] == 2);
        REQUIRE(ReadAll(*ext4, first) == first_text);
        REQUIRE(ReadAll(*ext4, second) == second_text);
    }
    CheckClean(*device, "extent-grow");
    REQUIRE(free - after_sequential == (sequential.size() + BlockSize - 1) / BlockSize +
                                           TreeBlocks(*device, "/sequential.bin"));
    REQUIRE(Leaves(*device, "/sequential.bin").back().length > 2000);
    REQUIRE(TreeDepth(*device, "/first.bin") == 2);
    REQUIRE(Leaves(*device, "/first.bin").size() >= 400);
    CheckDebugfsCat(*device, "/first.bin", first_text);
    CheckDebugfsCat(*device, "/second.bin", second_text);
    {
        // Removing them gives back every data and tree block.
        MountedFilesystem ext4(Ext2Type(), *device);
        REQUIRE(ReadAll(*ext4, Value(Walk(*ext4, "second.bin"))) == second_text);
        Succeeds(ext4->Remove(ext4->Root(), "first.bin"));
        Succeeds(ext4->Remove(ext4->Root(), "second.bin"));
        Succeeds(ext4->Remove(ext4->Root(), "sequential.bin"));
        REQUIRE(FreeBlocks(*ext4) == free);
    }
    CheckClean(*device, "extent-grow-removed");
#endif
}

TEST_CASE("ext4 fills holes before, between and after extents of a two-level tree")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage("ext4-nojournal"));
    REQUIRE(TreeDepth(*device, "/holes.bin") == 2);
    std::string holes;
    std::string spread;
    {
        MountedFilesystem ext4(Ext2Type(), *device);
        // Every eighth block of holes.bin is a hole: filling them inserts an extent in a full
        // leaf after another, splitting leaves and the index above them.
        const NodeId file = Value(Walk(*ext4, "holes.bin"));
        holes = ReadAll(*ext4, file);
        for (std::uint64_t block = 5; block * BlockSize < holes.size(); block += 8)
        {
            const std::string data = NumberedName("hole", static_cast<int>(block), 700);
            REQUIRE(WriteAt(*ext4, file, block * BlockSize + 200, data) == data.size());
            Overwrite(holes, block * BlockSize + 200, data);
        }
        REQUIRE(ReadAll(*ext4, file) == holes);

        // A file whose first extent starts far in: a block in front of it changes the first
        // logical block of every node on the left edge of the tree.
        const NodeId later = Value(ext4->Create(ext4->Root(), "spread.bin", 0644));
        for (int index = 0; index < 400; ++index)
        {
            const std::string data = NumberedName("spread", index, 100);
            const std::uint64_t offset = (1000 + std::uint64_t{2} * index) * BlockSize;
            REQUIRE(WriteAt(*ext4, later, offset, data) == data.size());
            Overwrite(spread, offset, data);
        }
        REQUIRE(RootHeader(*device, later)[2] == 2);
        for (const std::uint64_t offset :
             {std::uint64_t{0}, std::uint64_t{500 * BlockSize + 7}, std::uint64_t{999 * BlockSize}})
        {
            REQUIRE(WriteAt(*ext4, later, offset, "front") == 5);
            Overwrite(spread, offset, "front");
            REQUIRE(ReadAll(*ext4, later) == spread);
        }
        // Past the end, after a hole.
        REQUIRE(WriteAt(*ext4, later, 4000 * BlockSize + 5, "end") == 3);
        Overwrite(spread, 4000 * BlockSize + 5, "end");
        REQUIRE(ReadAll(*ext4, later) == spread);
    }
    CheckClean(*device, "extent-holes");
    CheckDebugfsCat(*device, "/holes.bin", holes);
    CheckDebugfsCat(*device, "/spread.bin", spread);
    const std::vector<DumpedEntry> leaves = Leaves(*device, "/spread.bin");
    REQUIRE(leaves.front().logical == 0);
    REQUIRE(leaves.back().logical == 4000);
#endif
}

TEST_CASE("ext4 turns unwritten extents written as data reaches them, never showing old data")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage("ext4-nojournal"));
    {
        // The filler takes the single free blocks between the image's files first, so debugfs
        // finds a long run.
        MountedFilesystem ext4(Ext2Type(), *device);
        const NodeId filler = Value(ext4->Create(ext4->Root(), "filler.bin", 0644));
        REQUIRE(WriteAt(*ext4, filler, 0, Pattern(2 * 1024 * 1024)) == 2 * 1024 * 1024);
        const NodeId file = Value(ext4->Create(ext4->Root(), "allocated.bin", 0644));
        REQUIRE(WriteAt(*ext4, file, 0, "start") == 5);
    }
    // 2000 blocks debugfs allocates unwritten, their old contents 0xA5 everywhere, and a size
    // that covers 1500 of them.
    REQUIRE(ChangeWithDebugfs(*device, "fallocate", "fallocate /allocated.bin 1 2000").ran);
    REQUIRE(ChangeWithDebugfs(*device, "fallocate-size", "sif /allocated.bin size 1536000").ran);
    {
        std::vector<std::uint8_t> bytes = BytesOf(*device);
        for (const DumpedEntry& extent : Leaves(*device, "/allocated.bin"))
        {
            if (extent.unwritten)
            {
                std::fill_n(bytes.begin() +
                                static_cast<std::ptrdiff_t>(extent.physical * BlockSize),
                            extent.length * BlockSize, std::uint8_t{0xA5});
            }
        }
        std::copy(bytes.begin(), bytes.end(), device->Bytes().begin());
    }
    std::string model = "start";
    model.resize(1536000, '\0');
    {
        MountedFilesystem ext4(Ext2Type(), *device);
        const NodeId file = Value(Walk(*ext4, "allocated.bin"));
        REQUIRE(ReadAll(*ext4, file) == model);
        // Inside one block in the middle: three extents of one, with zeros around the bytes.
        REQUIRE(WriteAt(*ext4, file, 700 * BlockSize + 100, "middle") == 6);
        Overwrite(model, 700 * BlockSize + 100, "middle");
        REQUIRE(ReadAll(*ext4, file) == model);
        // Front to back from block 1: the written part grows, the unwritten one shrinks.
        for (std::uint64_t offset = BlockSize; offset < 300 * BlockSize; offset += 1500)
        {
            const std::string piece = Pattern(1500);
            REQUIRE(WriteAt(*ext4, file, offset, piece) == piece.size());
            Overwrite(model, offset, piece);
        }
        // Across the written block and into the unwritten ones on both sides.
        const std::string across = Pattern(5000);
        REQUIRE(WriteAt(*ext4, file, 698 * BlockSize + 10, across) == across.size());
        Overwrite(model, 698 * BlockSize + 10, across);
        // Past the size, into blocks still unwritten, then past them into a hole.
        REQUIRE(WriteAt(*ext4, file, 1800 * BlockSize + 1, "beyond") == 6);
        Overwrite(model, 1800 * BlockSize + 1, "beyond");
        REQUIRE(WriteAt(*ext4, file, 2100 * BlockSize, "hole") == 4);
        Overwrite(model, 2100 * BlockSize, "hole");
        REQUIRE(ReadAll(*ext4, file) == model);
        REQUIRE(model.find('\xA5') == std::string::npos);
    }
    CheckClean(*device, "extent-unwritten");
    CheckDebugfsCat(*device, "/allocated.bin", model);
    const std::vector<DumpedEntry> leaves = Leaves(*device, "/allocated.bin");
    REQUIRE(std::count_if(leaves.begin(), leaves.end(),
                          [](const DumpedEntry& entry) { return entry.unwritten; }) >= 3);
    // Blocks 1 to 299, written front to back, are one written extent by now.
    REQUIRE(std::any_of(
        leaves.begin(), leaves.end(), [](const DumpedEntry& entry)
        { return !entry.unwritten && entry.logical <= 1 && entry.logical + entry.length >= 300; }));
    {
        // Truncating inside an unwritten extent and then removing the file.
        MountedFilesystem ext4(Ext2Type(), *device);
        const NodeId file = Value(Walk(*ext4, "allocated.bin"));
        const std::uint32_t free = FreeBlocks(*ext4);
        Succeeds(ext4->Truncate(file, 1000 * BlockSize + 10));
        model.resize(1000 * BlockSize + 10);
        REQUIRE(ReadAll(*ext4, file) == model);
        REQUIRE(FreeBlocks(*ext4) >= free + 1000);
    }
    CheckClean(*device, "extent-unwritten-truncated");
    {
        MountedFilesystem ext4(Ext2Type(), *device);
        // unwritten.bin: one written block, then 39 unwritten ones of one block each.
        const NodeId image_file = Value(Walk(*ext4, "unwritten.bin"));
        std::string image_model = ReadAll(*ext4, image_file);
        for (const std::uint64_t offset : {std::uint64_t{BlockSize + 3}, std::uint64_t{20000}})
        {
            REQUIRE(WriteAt(*ext4, image_file, offset, "x") == 1);
            Overwrite(image_model, offset, "x");
        }
        REQUIRE(ReadAll(*ext4, image_file) == image_model);
        Succeeds(ext4->Remove(ext4->Root(), "allocated.bin"));
    }
    CheckClean(*device, "extent-unwritten-removed");
#endif
}

TEST_CASE("ext4 truncates extent trees at many points and grows them with holes")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage("ext4-nojournal"));
    std::string text;
    std::string other;
    std::uint32_t free = 0;
    NodeId file = 0;
    {
        MountedFilesystem ext4(Ext2Type(), *device);
        free = FreeBlocks(*ext4);
        file = Value(ext4->Create(ext4->Root(), "shrinking.bin", 0644));
        const NodeId partner = Value(ext4->Create(ext4->Root(), "partner.bin", 0644));
        Interleave(*ext4, file, partner, 420, text, other);
        // A longer extent at the end, then one more interleaved block.
        const std::string run = Pattern(30 * BlockSize);
        REQUIRE(WriteAt(*ext4, file, text.size(), run) == run.size());
        text += run;
    }
    REQUIRE(TreeDepth(*device, "/shrinking.bin") == 2);
    // Inside the long extent, at its start, inside a leaf, at a leaf's first block, in the first
    // leaf, inside the first block, and to nothing.
    for (const std::uint64_t size :
         {std::uint64_t{text.size() - 3 * BlockSize - 17}, std::uint64_t{420 * BlockSize},
          std::uint64_t{300 * BlockSize + 5}, std::uint64_t{168 * BlockSize},
          std::uint64_t{50 * BlockSize}, std::uint64_t{1}, std::uint64_t{0}})
    {
        INFO("size " << size);
        {
            MountedFilesystem ext4(Ext2Type(), *device);
            const std::uint32_t before = FreeBlocks(*ext4);
            Succeeds(ext4->Truncate(file, size));
            REQUIRE(Value(ext4->GetAttributes(file)).size == size);
            REQUIRE(ReadAll(*ext4, file) == text.substr(0, size));
            REQUIRE(FreeBlocks(*ext4) >= before);
        }
        CheckClean(*device, "extent-truncate");
        CheckDebugfsCat(*device, "/shrinking.bin", text.substr(0, size));
    }
    REQUIRE(RootHeader(*device, file) == std::array<std::uint16_t, 3>{0, 4, 0});
    {
        MountedFilesystem ext4(Ext2Type(), *device);
        // Growing allocates nothing; what was cut off reads as zeros.
        REQUIRE(WriteAt(*ext4, file, 0, text.substr(0, 5000)) == 5000);
        Succeeds(ext4->Truncate(file, 3000));
        const std::uint32_t before = FreeBlocks(*ext4);
        Succeeds(ext4->Truncate(file, 1024 * 1024));
        REQUIRE(FreeBlocks(*ext4) == before);
        REQUIRE(ReadAll(*ext4, file) ==
                text.substr(0, 3000) + std::string(1024 * 1024 - 3000, '\0'));
        REQUIRE(WriteAt(*ext4, file, 1024 * 1024, "tail") == 4);
        REQUIRE(ErrorOf(ext4->Truncate(file, std::uint64_t{1} << 50)) == Error::FileTooLarge);
        // The image's tree two levels deep, cut in its middle and then emptied.
        const NodeId holes = Value(Walk(*ext4, "holes.bin"));
        const std::string holes_text = ReadAll(*ext4, holes);
        Succeeds(ext4->Truncate(holes, 1500 * BlockSize + 3));
        REQUIRE(ReadAll(*ext4, holes) == holes_text.substr(0, 1500 * BlockSize + 3));
        Succeeds(ext4->Truncate(holes, 0));
        Succeeds(ext4->Remove(ext4->Root(), "partner.bin"));
        Succeeds(ext4->Remove(ext4->Root(), "shrinking.bin"));
        Succeeds(ext4->Remove(ext4->Root(), "holes.bin"));
        REQUIRE(FreeBlocks(*ext4) > free);
    }
    CheckClean(*device, "extent-truncate-grow");
#endif
}

TEST_CASE("ext4 directories on extents grow to thousands of entries and lose hash indexes")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const std::string_view variant : {"ext4-nojournal", "ext4-mixed"})
    {
        INFO(variant);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(variant));
        // ext4-nojournal has inodes for 2811 more files.
        constexpr int Count = 2000;
        std::uint32_t free = 0;
        {
            MountedFilesystem ext4(Ext2Type(), *device);
            free = FreeBlocks(*ext4);
            const NodeId directory = Value(ext4->MakeDirectory(ext4->Root(), "wide", 0755));
            for (int number = 0; number < Count; ++number)
            {
                const NodeId made =
                    Value(ext4->Create(directory, NumberedName("file", number, 60), 0644));
                if (number % 100 == 0)
                {
                    REQUIRE(WriteAt(*ext4, made, 0, NumberedName("data", number, 3000)) == 3000);
                }
            }
            REQUIRE(List(*ext4, directory).size() == Count);
            // many/ (block-mapped or extents, with a hash index) takes a new name too.
            const NodeId many = Value(Walk(*ext4, "many"));
            Value(ext4->Create(many, "added-to-many.txt", 0644));
            REQUIRE(Value(ext4->Lookup(many, "entry-with-a-longer-name-150.txt")) != 0);
        }
        CheckClean(*device, std::string(variant) + "-wide");
        REQUIRE(Leaves(*device, "/wide").size() >= 1);
        {
            MountedFilesystem ext4(Ext2Type(), *device);
            const NodeId directory = Value(Walk(*ext4, "wide"));
            REQUIRE(
                ReadAll(*ext4, Value(ext4->Lookup(directory, NumberedName("file", 1900, 60)))) ==
                NumberedName("data", 1900, 3000));
            for (int number = 0; number < Count; ++number)
            {
                Succeeds(ext4->Remove(directory, NumberedName("file", number, 60)));
            }
            Succeeds(ext4->RemoveDirectory(ext4->Root(), "wide"));
            Succeeds(ext4->Remove(Value(Walk(*ext4, "many")), "added-to-many.txt"));
            // many/ keeps a block its new name may have added.
            REQUIRE(FreeBlocks(*ext4) <= free);
            REQUIRE(FreeBlocks(*ext4) + 1 >= free);
        }
        CheckClean(*device, std::string(variant) + "-wide-removed");
    }
    // large/ of ext4-nojournal: 3000 entries with a two-level hash index over an extent tree.
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage("ext4-nojournal"));
    {
        MountedFilesystem ext4(Ext2Type(), *device);
        const NodeId large = Value(Walk(*ext4, "large"));
        for (int number = 0; number < 200; ++number)
        {
            Value(ext4->Create(large, NumberedName("added", number, 100), 0644));
        }
        REQUIRE(List(*ext4, large).size() == 3200);
    }
    CheckClean(*device, "extent-large-directory");
#endif
}

#ifdef FEATHERDISK_EXT_IMAGES
namespace
{

// Ignores every failure: after the cut, everything that writes fails.
void ExtentCrashScenario(Filesystem& ext4)
{
    const NodeId root = ext4.Root();
    // The root's four extents, then a fifth that moves them into a leaf, and one in front of
    // them all, which gives the leaf a new first block.
    if (const Result<NodeId> file = ext4.Create(root, "deeper.bin", 0644))
    {
        for (int index = 1; index < 7; ++index)
        {
            static_cast<void>(ext4.Write(*file, std::uint64_t{2} * index * BlockSize,
                                         BytesOfText(NumberedName("piece", index, 300))));
        }
        static_cast<void>(ext4.Write(*file, 0, BytesOfText("front")));
    }
    // Holes in a full leaf of holes.bin, which splits.
    if (const Result<NodeId> holes = Walk(ext4, "holes.bin"))
    {
        for (const std::uint64_t block : {5, 13, 21, 29})
        {
            static_cast<void>(ext4.Write(*holes, block * BlockSize, BytesOfText(Block('h'))));
        }
        static_cast<void>(ext4.Truncate(*holes, 1000 * BlockSize + 1));
    }
    // A long unwritten extent written in its middle, then from its start on.
    if (const Result<NodeId> unwritten = Walk(ext4, "long.bin"))
    {
        static_cast<void>(ext4.Write(*unwritten, 100 * BlockSize + 10, BytesOfText("u")));
        static_cast<void>(ext4.Write(*unwritten, BlockSize, BytesOfText(Pattern(3 * BlockSize))));
    }
    static_cast<void>(ext4.Remove(root, "fragmented.bin"));
    if (const Result<NodeId> directory = ext4.MakeDirectory(root, "spill", 0755))
    {
        for (int number = 0; number < 30; ++number)
        {
            static_cast<void>(ext4.Create(*directory, NumberedName("spill", number, 120), 0644));
        }
    }
}

} // namespace
#endif

TEST_CASE("an ext4 extent volume stopped after any write needs only e2fsck's routine repairs")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    if (!HaveE2fsck())
    {
        SKIP("e2fsck is not installed");
    }
    // long.bin: a written block, then 300 debugfs allocated unwritten, after a filler that took
    // the image's single free blocks.
    std::vector<std::uint8_t> original = LoadExtImage("ext4-nojournal");
    {
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(original);
        const std::string filler = Pattern(2 * 1024 * 1024);
        REQUIRE(ScriptWithDebugfs(*device, "extent-crash-prepare",
                                  "write %0 /filler.bin\nwrite %1 /long.bin\n"
                                  "fallocate /long.bin 1 300\nsif /long.bin size 307200\n",
                                  {std::vector<std::uint8_t>(filler.begin(), filler.end()),
                                   std::vector<std::uint8_t>{'x'}})
                    .ran);
        const std::vector<DumpedEntry> leaves = Leaves(*device, "/long.bin");
        REQUIRE(std::any_of(leaves.begin(), leaves.end(), [](const DumpedEntry& entry)
                            { return entry.unwritten && entry.length > 200; }));
        original = BytesOf(*device);
    }
    std::uint64_t total = 0;
    {
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(original);
        {
            MountedFilesystem ext4(Ext2Type(), *device);
            ExtentCrashScenario(*ext4);
            REQUIRE(RootHeader(*device, Value(Walk(*ext4, "deeper.bin")))[2] == 1);
            REQUIRE(ReadAt(*ext4, Value(Walk(*ext4, "deeper.bin")), 0, 6) ==
                    std::string("front") + '\0');
            REQUIRE(ReadAt(*ext4, Value(Walk(*ext4, "long.bin")), 100 * BlockSize + 9, 3) ==
                    std::string("\0u\0", 3));
        }
        total = device->write_count;
        CheckClean(*device, "extent-crash-complete");
        const std::vector<DumpedEntry> leaves = Leaves(*device, "/long.bin");
        REQUIRE(std::count_if(leaves.begin(), leaves.end(),
                              [](const DumpedEntry& entry) { return entry.unwritten; }) == 2);
    }
    REQUIRE(total > 50);
    for (std::uint64_t cut = 1; cut < total; ++cut)
    {
        INFO("stopped after write " << cut << " of " << total);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(original);
        device->FailWritesAfter(cut);
        const Result<Filesystem*> mounted = Ext2Type().Mount(*device, {.read_only = false});
        REQUIRE(mounted.has_value());
        ExtentCrashScenario(**mounted);
        const std::unique_ptr<MemoryDevice> stopped = CopyOf(*device);
        device->FailWritesAfter(UINT64_MAX);
        Succeeds(Ext2Type().Unmount(**mounted));
        const ToolRun report = RunE2fsck(*stopped, "extent-crash");
        INFO(report.output);
        REQUIRE(OnlyRoutineRepairs(report.output));
    }
#endif
}

TEST_CASE("random writes and truncations of one extent file match a model")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const std::string_view variant : {"ext4-nojournal", "ext4-mixed"})
    {
        INFO(variant);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(variant));
        std::mt19937 random(variant == "ext4-mixed" ? 77 : 78);
        std::string model;
        {
            auto ext4 = std::make_unique<MountedFilesystem>(Ext2Type(), *device);
            NodeId file = Value((*ext4)->Create((*ext4)->Root(), "random.bin", 0644));
            for (int operation = 0; operation < 600; ++operation)
            {
                INFO("operation " << operation);
                const std::uint32_t choice = random() % 100;
                if (choice < 85)
                {
                    const std::uint64_t offset = random() % (3 * 1024 * 1024);
                    const std::size_t length = 1 + random() % (random() % 8 == 0 ? 60000 : 3000);
                    const std::string data = NumberedName("r", operation, length);
                    REQUIRE(WriteAt(**ext4, file, offset, data) == data.size());
                    Overwrite(model, offset, data);
                }
                else if (choice < 95)
                {
                    const std::uint64_t size =
                        model.empty() ? 0 : random() % (model.size() + model.size() / 4);
                    Succeeds((*ext4)->Truncate(file, size));
                    model.resize(static_cast<std::size_t>(size), '\0');
                }
                else
                {
                    ext4.reset();
                    ext4 = std::make_unique<MountedFilesystem>(Ext2Type(), *device);
                    file = Value(Walk(**ext4, "random.bin"));
                }
                if (operation % 50 == 0)
                {
                    REQUIRE(ReadAll(**ext4, file) == model);
                }
            }
            REQUIRE(ReadAll(**ext4, file) == model);
        }
        CheckClean(*device, std::string(variant) + "-extent-random");
        CheckDebugfsCat(*device, "/random.bin", model);
    }
#endif
}
