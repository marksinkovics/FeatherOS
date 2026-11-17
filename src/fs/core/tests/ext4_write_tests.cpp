// SPDX-License-Identifier: MIT OR Apache-2.0
// Writes on ext4 layouts without a journal or extents (tests/make_ext2_image.sh): which volumes
// take them, groups that were never used, bitmaps that fail their checksums, hashed directories,
// dir_nlink, huge_file and shared extended attribute blocks. The scenarios of ext2_write_tests.cpp
// and ext2_tree_tests.cpp run over these layouts too.
#include <catch2/catch_test_macros.hpp>

#include "ext/checksum.h"
#include "ext/inode.h"
#include "ext/superblock.h"
#include "fs/ext2.h"
#include "tests/ext2_image.h"
#include "tests/filesystem_helpers.h"
#include "tests/memory_device.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
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
using FeatherDisk::Ext::AttributeBlockChecksum;
using FeatherDisk::Ext::DecodeGroupDescriptor;
using FeatherDisk::Ext::DecodeSuperblock;
using FeatherDisk::Ext::Ext2Type;
using FeatherDisk::Ext::GroupBlocksUnused;
using FeatherDisk::Ext::GroupDescriptor;
using FeatherDisk::Ext::GroupInodesUnused;
using FeatherDisk::Ext::InodeHugeFile;
using FeatherDisk::Ext::InodeIndexed;
using FeatherDisk::Ext::StateErrors;
using FeatherDisk::Ext::Superblock;
using FeatherDisk::Ext::VolumeChecksumSeed;
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
using FeatherDisk::Tests::PreenWithE2fsck;
using FeatherDisk::Tests::RawVolume;
using FeatherDisk::Tests::ReadAll;
using FeatherDisk::Tests::RunDebugfs;
using FeatherDisk::Tests::RunE2fsck;
using FeatherDisk::Tests::Succeeds;
using FeatherDisk::Tests::ToolRun;
using FeatherDisk::Tests::Value;
using FeatherDisk::Tests::Walk;
using FeatherDisk::Tests::WriteAt;
using FeatherDisk::Tests::WriteNewFile;

constexpr std::uint64_t SuperblockStart = 1024;
constexpr std::uint64_t StateOffset = SuperblockStart + 58;
constexpr std::uint64_t CompatibleOffset = SuperblockStart + 92;
constexpr std::uint64_t IncompatibleOffset = SuperblockStart + 96;
constexpr std::uint64_t ReadOnlyCompatibleOffset = SuperblockStart + 100;
constexpr std::uint32_t RootNode = 2;
constexpr int HashedNames = 3000;

Superblock SuperblockOf(const std::vector<std::uint8_t>& bytes)
{
    return DecodeSuperblock(std::span(bytes).subspan(SuperblockStart).first<1024>());
}

GroupDescriptor DescriptorOf(RawVolume& raw, std::uint32_t group)
{
    return DecodeGroupDescriptor(std::span(raw.bytes).subspan(
        raw.DescriptorOffset() + group * raw.DescriptorSize(), raw.DescriptorSize()));
}

std::uint32_t InodeOf(RawVolume& raw, std::uint32_t directory, std::string_view name)
{
    return raw.Load32(raw.EntryOffset(directory, name));
}

std::uint32_t InodeFlags(MemoryDevice& device, NodeId node)
{
    std::vector<std::uint8_t> bytes = BytesOf(device);
    RawVolume raw{bytes};
    return raw.Load32(raw.InodeOffset(static_cast<std::uint32_t>(node)) + 32);
}

std::string HashedName(int number)
{
    std::array<char, 64> name{};
    std::snprintf(name.data(), name.size(), "file-with-a-long-name-for-htree-%05d.txt", number);
    return name.data();
}

void CheckDebugfsHas(MemoryDevice& device, std::string_view command,
                     const std::vector<std::string>& expected)
{
    const ToolRun run = RunDebugfs(device, "ext4-write", command);
    if (!run.ran)
    {
        return;
    }
    INFO("debugfs -R '" << command << "':\n" << run.output);
    for (const std::string& text : expected)
    {
        REQUIRE(run.output.find(text) != std::string::npos);
    }
}

} // namespace
#endif

TEST_CASE("ext4 volumes without a journal or extents mount writable, the others read-only")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    const auto writable = [](std::string_view variant, auto change)
    {
        std::vector<std::uint8_t> bytes = LoadExtImage(variant);
        RawVolume raw{bytes};
        change(raw);
        if ((raw.Load32(ReadOnlyCompatibleOffset) & 0x400U) != 0)
        {
            raw.UpdateSuperblockChecksum();
        }
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
        bool result = false;
        {
            MountedFilesystem ext4(Ext2Type(), *device);
            result = ext4->GetCapabilities().writable;
            if (!result)
            {
                REQUIRE(ErrorOf(ext4->Create(ext4->Root(), "x", 0644)) == Error::ReadOnly);
            }
        }
        if (!result)
        {
            REQUIRE(device->write_count == 0);
        }
        return result;
    };
    const auto unchanged = [](RawVolume&) {};
    for (const std::string_view variant :
         {"ext4-csum", "ext4-csum64", "ext4-gdtcsum", "ext4-htree", "ext4-htree-gdtcsum"})
    {
        INFO(variant);
        REQUIRE(writable(variant, unchanged));
    }
    // A journal (and orphan_file with it), extents and a journal.
    REQUIRE_FALSE(writable("ext4-blockmap", unchanged));
    REQUIRE_FALSE(writable("ext4", unchanged));
    REQUIRE_FALSE(writable("ext4-extents", unchanged));
    // The extent feature alone, with no file using it yet.
    REQUIRE_FALSE(
        writable("ext4-csum", [](RawVolume& raw)
                 { raw.Store32(IncompatibleOffset, raw.Load32(IncompatibleOffset) | 0x40U); }));
    // orphan_file without a journal, and quota, whose rules the driver does not keep.
    REQUIRE_FALSE(
        writable("ext4-csum", [](RawVolume& raw)
                 { raw.Store32(CompatibleOffset, raw.Load32(CompatibleOffset) | 0x1000U); }));
    REQUIRE_FALSE(writable(
        "ext4-csum", [](RawVolume& raw)
        { raw.Store32(ReadOnlyCompatibleOffset, raw.Load32(ReadOnlyCompatibleOffset) | 0x100U); }));
#endif
}

TEST_CASE("ext4 writes initialise groups whose bitmaps and inode tables were never written")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const std::string_view variant : {"ext4-csum", "ext4-csum64", "ext4-gdtcsum"})
    {
        INFO(variant);
        std::vector<std::uint8_t> original = LoadExtImage(variant);
        RawVolume before{original};
        const Superblock layout = SuperblockOf(original);
        const std::uint32_t per_group = layout.inodes_per_group;
        const auto group_of_block = [&](std::uint64_t block)
        {
            return static_cast<std::uint32_t>((block - layout.first_data_block) /
                                              layout.blocks_per_group);
        };
        std::uint32_t uninitialised_blocks = 0;
        for (std::uint32_t group = 1; group < layout.group_count; ++group)
        {
            REQUIRE((DescriptorOf(before, group).flags & GroupInodesUnused) != 0);
            REQUIRE(DescriptorOf(before, group).unused_inodes == per_group);
            uninitialised_blocks +=
                (DescriptorOf(before, group).flags & GroupBlocksUnused) != 0 ? 1 : 0;
        }
        REQUIRE(uninitialised_blocks > 0);

        const std::unique_ptr<MemoryDevice> device = DeviceFrom(original);
        std::vector<NodeId> tops;
        {
            MountedFilesystem ext4(Ext2Type(), *device);
            // Top-level directories spread over the groups (Orlov-lite), so each takes the
            // first inode of an unused table and a block in its group.
            for (std::uint32_t group = 1; group < layout.group_count; ++group)
            {
                const std::string name = "spread-" + std::to_string(group);
                tops.push_back(Value(ext4->MakeDirectory(ext4->Root(), name, 0755)));
                WriteNewFile(*ext4, name + "/inside.txt", Pattern(3000) + name);
            }
        }
        CheckClean(*device, std::string(variant) + "-uninit");
        std::vector<std::uint8_t> bytes = BytesOf(*device);
        RawVolume after{bytes};
        std::uint32_t blocks_initialised = 0;
        for (const NodeId top : tops)
        {
            const auto group = static_cast<std::uint32_t>((top - 1) / per_group);
            INFO("inode " << top << " in group " << group);
            REQUIRE(group > 0);
            const GroupDescriptor descriptor = DescriptorOf(after, group);
            REQUIRE((descriptor.flags & GroupInodesUnused) == 0);
            // The directory and its file: the table is in use up to the second inode.
            REQUIRE(descriptor.unused_inodes == per_group - 2);
            REQUIRE(descriptor.free_inodes == per_group - 2);
            const std::uint32_t block_group =
                group_of_block(after.InodeBlock(static_cast<std::uint32_t>(top), 0));
            REQUIRE(block_group == group);
            if ((DescriptorOf(before, group).flags & GroupBlocksUnused) != 0)
            {
                REQUIRE((descriptor.flags & GroupBlocksUnused) == 0);
                ++blocks_initialised;
            }
        }
        REQUIRE(blocks_initialised == uninitialised_blocks);
        MountedFilesystem ext4(Ext2Type(), *device, true);
        for (std::uint32_t group = 1; group < layout.group_count; ++group)
        {
            const std::string name = "spread-" + std::to_string(group);
            REQUIRE(ReadAll(*ext4, Value(Walk(*ext4, name + "/inside.txt"))) ==
                    Pattern(3000) + name);
        }
    }
#endif
}

TEST_CASE("an ext4 bitmap that fails its checksum stops writes until e2fsck has run")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const bool blocks : {true, false})
    {
        INFO((blocks ? "block bitmap" : "inode bitmap"));
        std::vector<std::uint8_t> bytes = LoadExtImage("ext4-csum");
        RawVolume raw{bytes};
        const GroupDescriptor group = DescriptorOf(raw, 0);
        // A bit past everything in use, so only the checksum is wrong.
        const std::uint64_t bitmap = raw.BlockOffset(
            static_cast<std::uint32_t>(blocks ? group.block_bitmap : group.inode_bitmap));
        bytes[bitmap + (blocks ? 200 : 60)] ^= 0x80;
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
        {
            MountedFilesystem ext4(Ext2Type(), *device);
            REQUIRE(ext4->GetCapabilities().writable);
            if (blocks)
            {
                const NodeId file = Value(ext4->Create(ext4->Root(), "new.txt", 0644));
                REQUIRE(ErrorOf(ext4->Write(file, 0, BytesOfText("x"))) == Error::Corrupt);
            }
            else
            {
                REQUIRE(ErrorOf(ext4->Create(ext4->Root(), "new.txt", 0644)) == Error::Corrupt);
            }
            REQUIRE_FALSE(ext4->GetCapabilities().writable);
            REQUIRE(ErrorOf(ext4->Create(ext4->Root(), "other.txt", 0644)) == Error::ReadOnly);
            REQUIRE(ReadAll(*ext4, Value(Walk(*ext4, "hello.txt"))) == "Hello from ext2.\n");
        }
        // The errors state stays for e2fsck, and keeps the next mount read-only.
        std::vector<std::uint8_t> written = BytesOf(*device);
        REQUIRE((RawVolume{written}.Load16(StateOffset) & StateErrors) != 0);
        MountedFilesystem again(Ext2Type(), *device);
        REQUIRE_FALSE(again->GetCapabilities().writable);
    }
#endif
}

TEST_CASE("a hashed ext4 directory keeps its index while names leave and drops it for a new one")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    // Every directory operation walks all 190 blocks, so the test touches a sample of the names.
    const auto removed = [](int number) { return number % 5 == 0; };
    for (const std::string_view variant : {"ext4-htree", "ext4-htree-gdtcsum"})
    {
        INFO(variant);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(variant));
        NodeId large = 0;
        {
            MountedFilesystem ext4(Ext2Type(), *device);
            const NodeId root = ext4->Root();
            large = Value(Walk(*ext4, "large"));
            for (int number = 0; number < HashedNames; number += 5)
            {
                Succeeds(ext4->Remove(large, HashedName(number)));
            }
            // Replacing a name changes no hash, and moving the directory changes the ".." in
            // its index root: the index stays either way.
            WriteNewFile(*ext4, "replacement.txt", "replaced\n");
            Succeeds(ext4->Rename(root, "replacement.txt", large, HashedName(1)));
            const NodeId a = Value(Walk(*ext4, "a"));
            Succeeds(ext4->Rename(root, "large", a, "large"));
            REQUIRE(Value(ext4->Lookup(large, "..")) == a);
            Succeeds(ext4->Rename(a, "large", root, "large"));
        }
        REQUIRE((InodeFlags(*device, large) & InodeIndexed) != 0);
        CheckClean(*device, std::string(variant) + "-htree-removed");
        CheckDebugfsHas(*device, "htree /large", {"Indirect levels: 1"});
        const int kept = HashedNames - HashedNames / 5;
        constexpr int Added = 100;
        {
            MountedFilesystem ext4(Ext2Type(), *device);
            for (int number = 0; number < HashedNames; number += 7)
            {
                REQUIRE(ext4->Lookup(large, HashedName(number)).has_value() != removed(number));
            }
            REQUIRE(List(*ext4, large).size() == static_cast<std::size_t>(kept));
            REQUIRE(ReadAll(*ext4, Value(ext4->Lookup(large, HashedName(1)))) == "replaced\n");
            // A new name: the index goes first.
            for (int number = 0; number < Added; ++number)
            {
                WriteNewFile(*ext4, "large/" + NumberedName("added", number, 30), "added\n");
            }
            REQUIRE(List(*ext4, large).size() == static_cast<std::size_t>(kept + Added));
        }
        REQUIRE((InodeFlags(*device, large) & InodeIndexed) == 0);
        CheckClean(*device, std::string(variant) + "-htree-added");
        {
            MountedFilesystem ext4(Ext2Type(), *device);
            const std::uint64_t free_inodes = Value(ext4->Statistics()).free_nodes;
            int gone = 0;
            for (int number = 1; number < HashedNames; number += 7)
            {
                REQUIRE(ext4->Lookup(large, HashedName(number)).has_value() != removed(number));
                if (!removed(number))
                {
                    Succeeds(ext4->Remove(large, HashedName(number)));
                    ++gone;
                }
            }
            for (int number = 0; number < Added; ++number)
            {
                const std::string name = NumberedName("added", number, 30);
                REQUIRE(ReadAll(*ext4, Value(ext4->Lookup(large, name))) == "added\n");
                Succeeds(ext4->Remove(large, name));
            }
            REQUIRE(Value(ext4->Statistics()).free_nodes == free_inodes + gone + Added);
            REQUIRE(List(*ext4, large).size() == static_cast<std::size_t>(kept - gone));
        }
        CheckClean(*device, std::string(variant) + "-htree-emptied");
    }
#endif
}

TEST_CASE("an ext4 volume stopped while a directory drops its index needs only e2fsck's repairs")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    if (!HaveE2fsck())
    {
        SKIP("e2fsck is not installed");
    }
    const auto scenario = [](Filesystem& ext4)
    {
        const Result<NodeId> large = Walk(ext4, "large");
        if (!large)
        {
            return;
        }
        static_cast<void>(ext4.Remove(*large, HashedName(7)));
        for (int number = 0; number < 3; ++number)
        {
            static_cast<void>(ext4.Create(*large, NumberedName("added", number, 30), 0644));
        }
    };
    const std::vector<std::uint8_t> original = LoadExtImage("ext4-htree");
    std::uint64_t total = 0;
    {
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(original);
        {
            MountedFilesystem ext4(Ext2Type(), *device);
            scenario(*ext4);
        }
        total = device->write_count;
        CheckClean(*device, "htree-crash-complete");
    }
    for (std::uint64_t cut = 1; cut < total; ++cut)
    {
        INFO("stopped after write " << cut << " of " << total);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(original);
        device->FailWritesAfter(cut);
        const Result<Filesystem*> mounted = Ext2Type().Mount(*device, {.read_only = false});
        REQUIRE(mounted.has_value());
        scenario(**mounted);
        const std::unique_ptr<MemoryDevice> stopped = CopyOf(*device);
        device->FailWritesAfter(UINT64_MAX);
        Succeeds(Ext2Type().Unmount(**mounted));
        const ToolRun report = RunE2fsck(*stopped, "htree-crash");
        INFO(report.output);
        REQUIRE(OnlyRoutineRepairs(report.output, false, true));
        // And preen mode repairs all of it, as at boot.
        REQUIRE(PreenWithE2fsck(*stopped, "htree-crash").status <= 1);
        CheckClean(*stopped, "htree-crash-preened");
    }
#endif
}

TEST_CASE("ext4 with dir_nlink counts a directory past 65000 links as 1")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage("ext4-csum"));
    // debugfs keeps the inodes' checksums right.
    REQUIRE(ChangeWithDebugfs(*device, "nlink", "sif /a links_count 64999").status == 0);
    REQUIRE(ChangeWithDebugfs(*device, "nlink", "sif /hello.txt links_count 64999").status == 0);
    {
        MountedFilesystem ext4(Ext2Type(), *device);
        const NodeId root = ext4->Root();
        const NodeId a = Value(Walk(*ext4, "a"));
        const auto links = [&](NodeId node) { return Value(ext4->GetAttributes(node)).link_count; };
        Value(ext4->MakeDirectory(a, "the-65000th", 0755));
        REQUIRE(links(a) == 65000);
        Value(ext4->MakeDirectory(a, "past", 0755));
        REQUIRE(links(a) == 1);
        Value(ext4->MakeDirectory(a, "more", 0755));
        REQUIRE(links(a) == 1);
        // Once 1, it stays 1, as Linux keeps it.
        Succeeds(ext4->RemoveDirectory(a, "past"));
        REQUIRE(links(a) == 1);
        const std::uint32_t root_links = links(root);
        Value(ext4->MakeDirectory(root, "mover", 0755));
        Succeeds(ext4->Rename(root, "mover", a, "mover"));
        REQUIRE(links(a) == 1);
        REQUIRE(links(root) == root_links);
        Succeeds(ext4->Rename(a, "mover", root, "mover"));
        REQUIRE(links(a) == 1);
        REQUIRE(links(root) == root_links + 1);

        // Files may have 65000 links too.
        const NodeId hello = Value(Walk(*ext4, "hello.txt"));
        Succeeds(ext4->Link(hello, root, "link-65000"));
        REQUIRE(links(hello) == 65000);
        REQUIRE(ext4->Link(hello, root, "link-65001").error() == Error::TooManyLinks);
    }
    CheckDebugfsHas(*device, "stat /a", {"Links: 1"});
    CheckDebugfsHas(*device, "stat /hello.txt", {"Links: 65000"});
#endif
}

TEST_CASE("ext4 with huge_file stores i_blocks as Linux does, in blocks only past 48 bits")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage("ext4-csum"));
    std::vector<std::uint8_t> bytes = BytesOf(*device);
    RawVolume raw{bytes};
    const std::uint32_t big = InodeOf(raw, RootNode, "big.txt");
    const std::uint32_t sectors = raw.Load32(raw.InodeOffset(big) + 28);
    const std::string blocks = std::to_string(sectors / 2);
    // The same count in 1 KiB blocks under the huge-file flag, which e2fsck accepts.
    REQUIRE(ChangeWithDebugfs(*device, "huge", "sif /big.txt flags 0x40000").status == 0);
    REQUIRE(ChangeWithDebugfs(*device, "huge", "sif /big.txt blocks " + blocks).status == 0);
    REQUIRE(InodeFlags(*device, big) == InodeHugeFile);
    CheckClean(*device, "huge-file-flagged");
    std::string text;
    {
        MountedFilesystem ext4(Ext2Type(), *device);
        const std::uint32_t free = FreeBlocks(*ext4);
        text = ReadAll(*ext4, big);
        REQUIRE(text.size() == 448000);
        // 512 bytes fill the last block, 2000 more take two.
        const std::string more = Pattern(2512);
        REQUIRE(WriteAt(*ext4, big, text.size(), more) == more.size());
        text += more;
        REQUIRE(FreeBlocks(*ext4) == free - 2);
    }
    CheckClean(*device, "huge-file-written");
    REQUIRE(InodeFlags(*device, big) == 0);
    CheckDebugfsHas(*device, "stat /big.txt", {"Blockcount: " + std::to_string(sectors + 4)});
    MountedFilesystem ext4(Ext2Type(), *device, true);
    REQUIRE(ReadAll(*ext4, big) == text);
#endif
}

TEST_CASE("ext4 extended attribute blocks keep their checksum as their references drop")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage("ext4-csum"));
    // Too long for the room inside a 256-byte inode, so the attribute takes a block.
    const ToolRun set =
        ChangeWithDebugfs(*device, "xattr", "ea_set /hello.txt user.featherdisk " + Pattern(300));
    if (!set.ran)
    {
        SKIP("debugfs is not installed");
    }
    std::vector<std::uint8_t> bytes = BytesOf(*device);
    RawVolume raw{bytes};
    const std::uint32_t hello = InodeOf(raw, RootNode, "hello.txt");
    const std::uint32_t block = raw.Load32(raw.InodeOffset(hello) + 104);
    REQUIRE(block != 0);
    // tool shares the block: two references, the checksum made to match. Its data block and
    // the attribute block are four sectors.
    const std::uint32_t tool = InodeOf(raw, RootNode, "tool");
    const std::uint32_t tool_sectors = raw.Load32(raw.InodeOffset(tool) + 28);
    REQUIRE(
        ChangeWithDebugfs(*device, "xattr", "sif /tool file_acl " + std::to_string(block)).status ==
        0);
    REQUIRE(
        ChangeWithDebugfs(*device, "xattr", "sif /tool blocks " + std::to_string(tool_sectors + 2))
            .status == 0);
    bytes = BytesOf(*device);
    const std::uint64_t offset = raw.BlockOffset(block);
    raw.Store32(offset + 4, 2);
    const std::uint32_t seed = VolumeChecksumSeed(SuperblockOf(bytes));
    raw.Store32(offset + 16, AttributeBlockChecksum(
                                 seed, block, std::span(bytes).subspan(offset, raw.BlockSize())));
    const std::unique_ptr<MemoryDevice> shared = DeviceFrom(bytes);
    CheckClean(*shared, "xattr-shared");
    {
        MountedFilesystem ext4(Ext2Type(), *shared);
        const std::uint32_t free = FreeBlocks(*ext4);
        Succeeds(ext4->Remove(ext4->Root(), "hello.txt"));
        Succeeds(ext4->Remove(ext4->Root(), "hard.txt"));
        // The data block; the attribute block stays for tool.
        REQUIRE(FreeBlocks(*ext4) == free + 1);
    }
    CheckClean(*shared, "xattr-one-reference");
    {
        MountedFilesystem ext4(Ext2Type(), *shared);
        const std::uint32_t free = FreeBlocks(*ext4);
        Succeeds(ext4->Remove(ext4->Root(), "tool"));
        REQUIRE(FreeBlocks(*ext4) == free + 2);
    }
    CheckClean(*shared, "xattr-freed");

    // A block whose checksum does not match is not touched.
    std::vector<std::uint8_t> damaged = BytesOf(*device);
    RawVolume damaged_raw{damaged};
    damaged[damaged_raw.BlockOffset(block) + 40] ^= 1;
    const std::unique_ptr<MemoryDevice> broken = DeviceFrom(damaged);
    MountedFilesystem ext4(Ext2Type(), *broken);
    Succeeds(ext4->Remove(ext4->Root(), "hello.txt"));
    REQUIRE(ext4->Remove(ext4->Root(), "hard.txt").error() == Error::Corrupt);
    REQUIRE(Value(Walk(*ext4, "hard.txt")) != 0);
#endif
}
