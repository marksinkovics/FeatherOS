// SPDX-License-Identifier: MIT OR Apache-2.0
// ext2 directories, renames, symbolic and hard links over images that mke2fs made, each scenario
// checked by e2fsck -fn and read back through debugfs.
#include <catch2/catch_test_macros.hpp>

#include "fs/ext2.h"
#include "tests/ext2_image.h"
#include "tests/filesystem_helpers.h"
#include "tests/memory_device.h"

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifdef FEATHERDISK_EXT_IMAGES
namespace
{

using FeatherDisk::Attributes;
using FeatherDisk::Error;
using FeatherDisk::Filesystem;
using FeatherDisk::NodeId;
using FeatherDisk::NodeType;
using FeatherDisk::Result;
using FeatherDisk::Ext::Ext2Type;
using FeatherDisk::Tests::BytesOf;
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
using FeatherDisk::Tests::RunDebugfs;
using FeatherDisk::Tests::RunE2fsck;
using FeatherDisk::Tests::Sorted;
using FeatherDisk::Tests::Succeeds;
using FeatherDisk::Tests::ToolRun;
using FeatherDisk::Tests::Value;
using FeatherDisk::Tests::Walk;
using FeatherDisk::Tests::WriteNewFile;

constexpr std::uint32_t RootNode = 2;

std::uint64_t FreeInodes(Filesystem& filesystem)
{
    return Value(filesystem.Statistics()).free_nodes;
}

std::uint32_t Links(Filesystem& filesystem, NodeId node)
{
    return Value(filesystem.GetAttributes(node)).link_count;
}

std::string LinkTarget(Filesystem& filesystem, NodeId link)
{
    std::string target(4096, '\0');
    target.resize(Value(filesystem.ReadLink(link, std::span(target.data(), target.size()))));
    return target;
}

std::int64_t Seconds()
{
    return static_cast<std::int64_t>(std::time(nullptr));
}

// debugfs, an implementation independent of the core, prints what it reads for command.
void CheckDebugfsHas(MemoryDevice& device, std::string_view command,
                     const std::vector<std::string>& expected)
{
    const ToolRun run = RunDebugfs(device, "tree", command);
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

void MakeAndRemoveDirectories(std::string_view variant)
{
    INFO(variant);
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(variant));
    std::uint32_t free_blocks = 0;
    std::uint64_t free_inodes = 0;
    std::uint32_t root_links = 0;
    std::uint64_t wide_size = 0;
    constexpr int WideCount = 120;
    constexpr int Depth = 10;
    {
        MountedFilesystem ext2(Ext2Type(), *device);
        const NodeId root = ext2->Root();
        const std::uint32_t block_size = Value(ext2->Statistics()).block_size;
        free_blocks = FreeBlocks(*ext2);
        free_inodes = FreeInodes(*ext2);
        root_links = Links(*ext2, root);
        const std::int64_t before = Seconds();

        const NodeId made = Value(ext2->MakeDirectory(root, "made", 040750));
        const Attributes attributes = Value(ext2->GetAttributes(made));
        REQUIRE(attributes.type == NodeType::Directory);
        REQUIRE(attributes.mode == 0750);
        REQUIRE(attributes.link_count == 2);
        REQUIRE(attributes.size == block_size);
        REQUIRE(attributes.modify_time.seconds >= before);
        REQUIRE(Links(*ext2, root) == root_links + 1);
        REQUIRE(Value(ext2->GetAttributes(root)).modify_time.seconds >= before);
        REQUIRE(FreeBlocks(*ext2) == free_blocks - 1);
        REQUIRE(FreeInodes(*ext2) == free_inodes - 1);
        REQUIRE(Value(ext2->Lookup(made, ".")) == made);
        REQUIRE(Value(ext2->Lookup(made, "..")) == root);
        REQUIRE(List(*ext2, made).empty());

        REQUIRE(ErrorOf(ext2->MakeDirectory(root, "made", 0755)) == Error::Exists);
        REQUIRE(ErrorOf(ext2->MakeDirectory(root, "hello.txt", 0755)) == Error::Exists);
        REQUIRE(ErrorOf(ext2->MakeDirectory(root, ".", 0755)) == Error::Exists);
        REQUIRE(ErrorOf(ext2->MakeDirectory(root, "..", 0755)) == Error::Exists);
        REQUIRE(ErrorOf(ext2->MakeDirectory(root, "", 0755)) == Error::InvalidArgument);
        REQUIRE(ErrorOf(ext2->MakeDirectory(root, std::string(256, 'd'), 0755)) ==
                Error::NameTooLong);
        REQUIRE(ErrorOf(ext2->MakeDirectory(Value(Walk(*ext2, "hello.txt")), "x", 0755)) ==
                Error::NotDirectory);

        // Deeper than eight levels, with a file at the bottom.
        NodeId parent = made;
        std::string path = "made";
        for (int level = 0; level < Depth; ++level)
        {
            parent = Value(ext2->MakeDirectory(parent, "level-" + std::to_string(level), 0755));
            path += "/level-" + std::to_string(level);
        }
        WriteNewFile(*ext2, path + "/bottom.txt", "at the bottom\n");
        REQUIRE(ReadAll(*ext2, Value(Walk(*ext2, path + "/bottom.txt"))) == "at the bottom\n");
        REQUIRE(Links(*ext2, made) == 3);

        // Enough subdirectories, with files between them, to take several blocks.
        const NodeId wide = Value(ext2->MakeDirectory(root, "wide", 0755));
        for (int number = 0; number < WideCount; ++number)
        {
            Value(ext2->MakeDirectory(wide, NumberedName("subdirectory", number, 60), 0700));
            if (number % 10 == 0)
            {
                WriteNewFile(*ext2, "wide/" + NumberedName("file", number, 40), "between\n");
            }
        }
        wide_size = Value(ext2->GetAttributes(wide)).size;
        REQUIRE(wide_size >= 3 * block_size);
        REQUIRE(Links(*ext2, wide) == 2 + WideCount);
        REQUIRE(List(*ext2, wide).size() == WideCount + WideCount / 10);
        REQUIRE(Value(Walk(*ext2, "wide/" + NumberedName("subdirectory", 77, 60) + "/..")) == wide);
    }
    CheckClean(*device, std::string(variant) + "-mkdir");
    CheckDebugfsHas(*device, "stat /made", {"Type: directory", "Mode:  0750", "Links: 3"});
    CheckDebugfsHas(*device, "ls /made/level-0/level-1/level-2", {"level-3", ".."});
    CheckDebugfsHas(*device,
                    "cat /made/level-0/level-1/level-2/level-3/level-4/level-5/level-6/"
                    "level-7/level-8/level-9/bottom.txt",
                    {"at the bottom"});
    {
        MountedFilesystem ext2(Ext2Type(), *device);
        const NodeId root = ext2->Root();
        const NodeId wide = Value(Walk(*ext2, "wide"));
        REQUIRE(ext2->RemoveDirectory(root, "made").error() == Error::NotEmpty);
        REQUIRE(ext2->RemoveDirectory(root, "hello.txt").error() == Error::NotDirectory);
        REQUIRE(ext2->RemoveDirectory(root, "fast-link").error() == Error::NotDirectory);
        REQUIRE(ext2->RemoveDirectory(root, "missing").error() == Error::NotFound);
        REQUIRE(ext2->RemoveDirectory(root, ".").error() == Error::InvalidArgument);
        REQUIRE(ext2->RemoveDirectory(wide, "..").error() == Error::InvalidArgument);
        REQUIRE(ext2->Remove(root, "made").error() == Error::IsDirectory);

        // Bottom up.
        std::vector<std::string> paths = {"made"};
        for (int level = 0; level < Depth; ++level)
        {
            paths.push_back(paths.back() + "/level-" + std::to_string(level));
        }
        Succeeds(ext2->Remove(Value(Walk(*ext2, paths.back())), "bottom.txt"));
        for (auto path = paths.rbegin(); path != paths.rend(); ++path)
        {
            const std::size_t slash = path->rfind('/');
            const NodeId parent =
                slash == std::string::npos ? root : Value(Walk(*ext2, path->substr(0, slash)));
            const std::uint32_t parent_links = Links(*ext2, parent);
            Succeeds(ext2->RemoveDirectory(
                parent, slash == std::string::npos ? *path : path->substr(slash + 1)));
            REQUIRE(Links(*ext2, parent) == parent_links - 1);
        }
        REQUIRE(ErrorOf(ext2->Lookup(root, "made")) == Error::NotFound);

        for (int number = 0; number < WideCount; ++number)
        {
            Succeeds(ext2->RemoveDirectory(wide, NumberedName("subdirectory", number, 60)));
            if (number % 10 == 0)
            {
                Succeeds(ext2->Remove(wide, NumberedName("file", number, 40)));
            }
        }
        // Removing entries leaves a directory its blocks, as Linux's ext2 does.
        REQUIRE(List(*ext2, wide).empty());
        REQUIRE(Links(*ext2, wide) == 2);
        REQUIRE(Value(ext2->GetAttributes(wide)).size == wide_size);
        Succeeds(ext2->RemoveDirectory(root, "wide"));
        REQUIRE(FreeBlocks(*ext2) == free_blocks);
        REQUIRE(FreeInodes(*ext2) == free_inodes);
        REQUIRE(Links(*ext2, root) == root_links);
    }
    CheckClean(*device, std::string(variant) + "-rmdir");
}

} // namespace
#endif

TEST_CASE("ext2 makes nested and wide directories and removes empty ones")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    MakeAndRemoveDirectories("1k");
    MakeAndRemoveDirectories("4k");
    MakeAndRemoveDirectories("nofiletype");
    MakeAndRemoveDirectories("ext4-csum");
    MakeAndRemoveDirectories("ext4-csum64");
    MakeAndRemoveDirectories("ext4-gdtcsum");
#endif
}

TEST_CASE("ext2 spreads top-level directories over groups and keeps others near their parent")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage("groups"));
    std::vector<std::uint8_t> original = BytesOf(*device);
    const RawVolume layout{original};
    const std::uint32_t inodes_per_group = layout.Load32(1024 + 40);
    const std::uint32_t blocks_per_group = layout.Load32(1024 + 32);
    const std::uint32_t first_data_block = layout.Load32(1024 + 20);
    REQUIRE(layout.BlockCount() / blocks_per_group >= 4);
    const auto group_of_inode = [&](NodeId node)
    { return static_cast<std::uint32_t>((node - 1) / inodes_per_group); };
    const auto group_of_block = [&](std::uint32_t block)
    { return (block - first_data_block) / blocks_per_group; };
    std::vector<NodeId> tops;
    std::vector<NodeId> inner;
    {
        MountedFilesystem ext2(Ext2Type(), *device);
        for (int number = 0; number < 4; ++number)
        {
            tops.push_back(
                Value(ext2->MakeDirectory(ext2->Root(), "top-" + std::to_string(number), 0755)));
        }
        for (const NodeId top : tops)
        {
            inner.push_back(Value(ext2->MakeDirectory(top, "inner", 0755)));
        }
        // Files stay with their directory too.
        const NodeId file = Value(ext2->Create(tops[2], "file.txt", 0644));
        REQUIRE(group_of_inode(file) == group_of_inode(tops[2]));
    }
    CheckClean(*device, "groups");
    std::vector<std::uint8_t> bytes = BytesOf(*device);
    const RawVolume raw{bytes};
    std::vector<std::uint32_t> groups;
    for (std::size_t index = 0; index < tops.size(); ++index)
    {
        const std::uint32_t group = group_of_inode(tops[index]);
        INFO("top-" << index << " is inode " << tops[index] << " in group " << group);
        REQUIRE(std::find(groups.begin(), groups.end(), group) == groups.end());
        groups.push_back(group);
        REQUIRE(group_of_inode(inner[index]) == group);
        // A directory's block is in its inode's group.
        REQUIRE(group_of_block(raw.InodeBlock(static_cast<std::uint32_t>(tops[index]), 0)) ==
                group);
        REQUIRE(group_of_block(raw.InodeBlock(static_cast<std::uint32_t>(inner[index]), 0)) ==
                group);
    }
#endif
}

#ifdef FEATHERDISK_EXT_IMAGES
namespace
{

void RenameAll(std::string_view variant)
{
    INFO(variant);
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(variant));
    {
        MountedFilesystem ext2(Ext2Type(), *device);
        const NodeId root = ext2->Root();
        const NodeId a = Value(Walk(*ext2, "a"));
        const NodeId b = Value(Walk(*ext2, "a/b"));
        const std::uint64_t free_inodes = FreeInodes(*ext2);
        const std::uint32_t root_links = Links(*ext2, root);
        const std::int64_t before = Seconds();

        // Within a directory, then across directories.
        const NodeId tool = Value(Walk(*ext2, "tool"));
        Succeeds(ext2->Rename(root, "tool", root, "renamed-tool"));
        REQUIRE(ErrorOf(ext2->Lookup(root, "tool")) == Error::NotFound);
        REQUIRE(Value(ext2->Lookup(root, "renamed-tool")) == tool);
        REQUIRE(Value(ext2->GetAttributes(tool)).change_time.seconds >= before);
        REQUIRE(Value(ext2->GetAttributes(tool)).mode == 04755);
        Succeeds(ext2->Rename(root, "renamed-tool", b, "tool"));
        REQUIRE(Value(Walk(*ext2, "a/b/tool")) == tool);
        REQUIRE(ReadAll(*ext2, tool) == "#!/bin/sh\n");
        REQUIRE(Value(ext2->GetAttributes(b)).modify_time.seconds >= before);
        REQUIRE(FreeInodes(*ext2) == free_inodes);

        // Over a file, which goes when that was its last name.
        WriteNewFile(*ext2, "source.txt", "source\n");
        WriteNewFile(*ext2, "a/b/target.txt", "target\n");
        Succeeds(ext2->Rename(root, "source.txt", b, "target.txt"));
        REQUIRE(ReadAll(*ext2, Value(Walk(*ext2, "a/b/target.txt"))) == "source\n");
        REQUIRE(ErrorOf(Walk(*ext2, "source.txt")) == Error::NotFound);
        REQUIRE(FreeInodes(*ext2) == free_inodes - 1);
        // Over one of two hard links: the other keeps the inode.
        const NodeId hello = Value(Walk(*ext2, "hello.txt"));
        WriteNewFile(*ext2, "over-hard.txt", "replacing\n");
        Succeeds(ext2->Rename(root, "over-hard.txt", root, "hard.txt"));
        REQUIRE(Links(*ext2, hello) == 1);
        REQUIRE(ReadAll(*ext2, hello) == "Hello from ext2.\n");
        REQUIRE(ReadAll(*ext2, Value(Walk(*ext2, "hard.txt"))) == "replacing\n");
        // Two names of one inode, and a name onto itself: nothing changes.
        const NodeId pair = WriteNewFile(*ext2, "pair-1.txt", "pair\n");
        Succeeds(ext2->Link(pair, root, "pair-2.txt"));
        Succeeds(ext2->Rename(root, "pair-1.txt", root, "pair-2.txt"));
        REQUIRE(Value(Walk(*ext2, "pair-1.txt")) == pair);
        REQUIRE(Value(Walk(*ext2, "pair-2.txt")) == pair);
        REQUIRE(Links(*ext2, pair) == 2);
        Succeeds(ext2->Rename(root, "pair-1.txt", root, "pair-1.txt"));
        REQUIRE(Value(Walk(*ext2, "pair-1.txt")) == pair);
        // A symbolic link moves as itself.
        Succeeds(ext2->Rename(root, "fast-link", a, "moved-link"));
        REQUIRE(LinkTarget(*ext2, Value(Walk(*ext2, "a/moved-link"))) == "a/b/c/deep.txt");

        // Kinds must match, and a replaced directory must be empty.
        Value(ext2->MakeDirectory(root, "empty-1", 0755));
        Value(ext2->MakeDirectory(root, "empty-2", 0700));
        REQUIRE(ext2->Rename(root, "empty-1", root, "hello.txt").error() == Error::NotDirectory);
        REQUIRE(ext2->Rename(root, "hello.txt", root, "empty-1").error() == Error::IsDirectory);
        REQUIRE(ext2->Rename(root, "empty-1", root, "a").error() == Error::NotEmpty);
        REQUIRE(ext2->Rename(root, "missing", root, "x").error() == Error::NotFound);
        REQUIRE(ext2->Rename(root, ".", root, "x").error() == Error::InvalidArgument);
        REQUIRE(ext2->Rename(root, "hello.txt", root, "..").error() == Error::InvalidArgument);
        REQUIRE(ext2->Rename(root, "hello.txt", root, std::string(256, 'r')).error() ==
                Error::NameTooLong);
        REQUIRE(ext2->Rename(root, "hello.txt", tool, "x").error() == Error::NotDirectory);
        // A directory into itself or below it.
        REQUIRE(ext2->Rename(root, "a", a, "inside").error() == Error::InvalidArgument);
        REQUIRE(ext2->Rename(root, "a", Value(Walk(*ext2, "a/b/c")), "inside").error() ==
                Error::InvalidArgument);
        REQUIRE(ext2->Rename(a, "b", Value(Walk(*ext2, "a/b/c")), "inside").error() ==
                Error::InvalidArgument);
        REQUIRE(Value(Walk(*ext2, "a/b/c/deep.txt")) != 0);

        // A directory over an empty one in the same directory: the parent loses a link.
        WriteNewFile(*ext2, "empty-1/inside.txt", "inside\n");
        const NodeId one = Value(Walk(*ext2, "empty-1"));
        const std::uint32_t links = Links(*ext2, root);
        Succeeds(ext2->Rename(root, "empty-1", root, "empty-2"));
        REQUIRE(Value(Walk(*ext2, "empty-2")) == one);
        REQUIRE(ReadAll(*ext2, Value(Walk(*ext2, "empty-2/inside.txt"))) == "inside\n");
        REQUIRE(Links(*ext2, root) == links - 1);

        // Across parents: ".." follows, and both parents' counts change.
        const NodeId moving = Value(ext2->MakeDirectory(root, "moving", 0755));
        Value(ext2->MakeDirectory(moving, "child", 0755));
        WriteNewFile(*ext2, "moving/file.txt", "moving\n");
        const std::uint32_t root_before = Links(*ext2, root);
        const std::uint32_t b_before = Links(*ext2, b);
        Succeeds(ext2->Rename(root, "moving", b, "moved"));
        REQUIRE(Value(ext2->Lookup(moving, "..")) == b);
        REQUIRE(Links(*ext2, root) == root_before - 1);
        REQUIRE(Links(*ext2, b) == b_before + 1);
        REQUIRE(Links(*ext2, moving) == 3);
        REQUIRE(Value(Walk(*ext2, "a/b/moved/child/..")) == moving);
        REQUIRE(ReadAll(*ext2, Value(Walk(*ext2, "a/b/moved/file.txt"))) == "moving\n");
        // And back up, over an empty directory: the new parent's count stays.
        Value(ext2->MakeDirectory(root, "landing", 0755));
        const std::uint32_t root_landing = Links(*ext2, root);
        Succeeds(ext2->Rename(b, "moved", root, "landing"));
        REQUIRE(Value(Walk(*ext2, "landing")) == moving);
        REQUIRE(Value(ext2->Lookup(moving, "..")) == root);
        REQUIRE(Links(*ext2, root) == root_landing);
        REQUIRE(Links(*ext2, b) == b_before);
        REQUIRE(Links(*ext2, root) == root_links + 2);
    }
    CheckClean(*device, std::string(variant) + "-rename");
    CheckDebugfsHas(*device, "cat /a/b/target.txt", {"source"});
    CheckDebugfsHas(*device, "ls /landing", {"child", "file.txt"});
    CheckDebugfsHas(*device, "ls /landing/child", {".."});
    {
        MountedFilesystem ext2(Ext2Type(), *device);
        // The old parent of a moved directory may not be its new parent's parent: a deep move
        // and back, then a rename in place whose new record takes the slack of the record before
        // the old one.
        const NodeId deep = Value(Walk(*ext2, "a/b/c"));
        Succeeds(
            ext2->Rename(Value(Walk(*ext2, "a/b")), "c", Value(Walk(*ext2, "landing/child")), "c"));
        REQUIRE(Value(ext2->Lookup(deep, "..")) == Value(Walk(*ext2, "landing/child")));
        Succeeds(ext2->Rename(Value(Walk(*ext2, "landing/child")), "c", ext2->Root(), "c"));
        const NodeId slack = Value(ext2->MakeDirectory(ext2->Root(), "slack", 0755));
        WriteNewFile(*ext2, "slack/first", "1");
        WriteNewFile(*ext2, "slack/middle", "2");
        WriteNewFile(*ext2, "slack/last", "3");
        Succeeds(ext2->Remove(slack, "middle"));
        Succeeds(ext2->Rename(slack, "last", slack, NumberedName("much-longer-name", 1, 60)));
        REQUIRE(Sorted(List(*ext2, slack)) ==
                std::vector<std::string>{"first", NumberedName("much-longer-name", 1, 60)});
        Succeeds(ext2->Rename(slack, "first", slack, NumberedName("much-longer-name", 1, 60)));
        REQUIRE(List(*ext2, slack) ==
                std::vector<std::string>{NumberedName("much-longer-name", 1, 60)});
        REQUIRE(Value(Walk(*ext2, "c/deep.txt")) != 0);
    }
    CheckClean(*device, std::string(variant) + "-rename-again");
}

} // namespace
#endif

TEST_CASE("ext2 renames files and directories as POSIX rename does")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    RenameAll("1k");
    RenameAll("nofiletype");
    RenameAll("ext4-csum");
    RenameAll("ext4-csum64");
#endif
}

TEST_CASE("ext2 stores short symbolic links in the inode and long ones in a block")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const std::string_view variant : {"1k", "4k", "ext4-csum", "ext4-gdtcsum"})
    {
        INFO(variant);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(variant));
        // Up to 59 bytes fit i_block's 60 with a terminator; 1023 is the most PATH_MAX allows,
        // and fills a 1 KiB block but for the terminator.
        const std::vector<std::size_t> lengths = {1, 58, 59, 60, 61, 200, 1023};
        const auto target_of = [](std::size_t length)
        {
            std::string target = Pattern(length);
            for (std::size_t index = 9; index < length; index += 10)
            {
                target[index] = '/';
            }
            return target;
        };
        std::uint32_t free_blocks = 0;
        std::uint64_t free_inodes = 0;
        {
            MountedFilesystem ext2(Ext2Type(), *device);
            const NodeId root = ext2->Root();
            free_blocks = FreeBlocks(*ext2);
            free_inodes = FreeInodes(*ext2);
            std::uint32_t slow = 0;
            for (const std::size_t length : lengths)
            {
                INFO("length " << length);
                const NodeId link = Value(
                    ext2->SymbolicLink(root, "link-" + std::to_string(length), target_of(length)));
                const Attributes attributes = Value(ext2->GetAttributes(link));
                REQUIRE(attributes.type == NodeType::SymbolicLink);
                REQUIRE(attributes.size == length);
                REQUIRE(attributes.mode == 0777);
                REQUIRE(attributes.link_count == 1);
                REQUIRE(LinkTarget(*ext2, link) == target_of(length));
                slow += length >= 60 ? 1 : 0;
                REQUIRE(FreeBlocks(*ext2) == free_blocks - slow);
                // A short buffer gets what fits.
                std::string part(5, '\0');
                REQUIRE(Value(ext2->ReadLink(link, std::span(part.data(), part.size()))) ==
                        std::min<std::size_t>(5, length));
            }
            REQUIRE(ErrorOf(ext2->SymbolicLink(root, "too-long", std::string(1024, 'l'))) ==
                    Error::NameTooLong);
            REQUIRE(ErrorOf(ext2->SymbolicLink(root, "empty", "")) == Error::InvalidArgument);
            REQUIRE(ErrorOf(ext2->SymbolicLink(root, "link-1", "x")) == Error::Exists);
            REQUIRE(ErrorOf(ext2->SymbolicLink(Value(Walk(*ext2, "hello.txt")), "x", "y")) ==
                    Error::NotDirectory);
            REQUIRE(ErrorOf(ext2->Read(Value(Walk(*ext2, "link-1")), 0, {})) ==
                    Error::InvalidArgument);
        }
        CheckClean(*device, std::string(variant) + "-symlink");
        CheckDebugfsHas(*device, "stat /link-59",
                        {"Type: symlink", "Fast link dest: \"" + target_of(59) + "\""});
        CheckDebugfsHas(*device, "stat /link-60", {"Type: symlink", "TOTAL: 1"});
        CheckDebugfsHas(*device, "cat /link-1023", {target_of(1023)});
        {
            MountedFilesystem ext2(Ext2Type(), *device, true);
            for (const std::size_t length : lengths)
            {
                REQUIRE(LinkTarget(*ext2, Value(Walk(*ext2, "link-" + std::to_string(length)))) ==
                        target_of(length));
            }
        }
        {
            MountedFilesystem ext2(Ext2Type(), *device);
            for (const std::size_t length : lengths)
            {
                Succeeds(ext2->Remove(ext2->Root(), "link-" + std::to_string(length)));
            }
            REQUIRE(FreeBlocks(*ext2) == free_blocks);
            REQUIRE(FreeInodes(*ext2) == free_inodes);
        }
        CheckClean(*device, std::string(variant) + "-symlink-removed");
    }

    // A slow link whose target fills its block exactly, which neither Linux nor e2fsck accept but
    // the reader must not overrun.
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage("1k"));
    NodeId link = 0;
    {
        MountedFilesystem ext2(Ext2Type(), *device);
        link = Value(ext2->SymbolicLink(ext2->Root(), "full", std::string(1023, 'f')));
    }
    std::vector<std::uint8_t> bytes = BytesOf(*device);
    RawVolume raw{bytes};
    const auto node = static_cast<std::uint32_t>(link);
    raw.Store32(raw.InodeOffset(node) + 4, 1024);
    bytes[raw.BlockOffset(raw.InodeBlock(node, 0)) + 1023] = 'g';
    const std::unique_ptr<MemoryDevice> patched = DeviceFrom(bytes);
    MountedFilesystem ext2(Ext2Type(), *patched, true);
    REQUIRE(LinkTarget(*ext2, link) == std::string(1023, 'f') + "g");
#endif
}

TEST_CASE("ext2 hard links share an inode until its last name goes")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage("1k"));
    const std::string text = Pattern(30 * 1024);
    {
        MountedFilesystem ext2(Ext2Type(), *device);
        const NodeId root = ext2->Root();
        const NodeId b = Value(Walk(*ext2, "a/b"));
        const std::uint64_t free_inodes = FreeInodes(*ext2);
        const NodeId file = WriteNewFile(*ext2, "original.txt", text);
        const std::int64_t before = Seconds();
        Succeeds(ext2->Link(file, b, "second.txt"));
        REQUIRE(Value(Walk(*ext2, "a/b/second.txt")) == file);
        REQUIRE(Links(*ext2, file) == 2);
        REQUIRE(Value(ext2->GetAttributes(file)).change_time.seconds >= before);
        REQUIRE(FreeInodes(*ext2) == free_inodes - 1);
        Succeeds(ext2->Remove(root, "original.txt"));
        REQUIRE(Links(*ext2, file) == 1);
        REQUIRE(ReadAll(*ext2, Value(Walk(*ext2, "a/b/second.txt"))) == text);
        REQUIRE(FreeInodes(*ext2) == free_inodes - 1);

        // Symbolic links and special nodes may have more names; directories may not.
        const NodeId link = Value(Walk(*ext2, "slow-link"));
        Succeeds(ext2->Link(link, root, "slow-link-2"));
        REQUIRE(LinkTarget(*ext2, Value(Walk(*ext2, "slow-link-2"))) ==
                "a/b/c/a-symbolic-link-target-that-is-longer-than-sixty-bytes.txt");
        Succeeds(ext2->Link(Value(Walk(*ext2, "pipe")), b, "pipe-2"));
        REQUIRE(ext2->Link(b, root, "b-again").error() == Error::NotPermitted);
        REQUIRE(ext2->Link(root, root, "root-again").error() == Error::NotPermitted);
        REQUIRE(ext2->Link(file, root, "hello.txt").error() == Error::Exists);
        REQUIRE(ext2->Link(file, root, ".").error() == Error::Exists);
        REQUIRE(ext2->Link(file, file, "x").error() == Error::NotDirectory);
        REQUIRE(ext2->Link(7, root, "resize").error() == Error::InvalidArgument);
    }
    CheckClean(*device, "hard-links");
    CheckDebugfsHas(*device, "stat /a/b/second.txt", {"Links: 1"});
    CheckDebugfsHas(*device, "stat /slow-link-2", {"Links: 2"});
    {
        const ToolRun run = RunDebugfs(*device, "hard-links", "cat /a/b/second.txt");
        if (run.ran)
        {
            REQUIRE(run.output == text);
        }
    }

    // ext2's limit of 32000 links, for files and for subdirectories.
    std::vector<std::uint8_t> bytes = BytesOf(*device);
    RawVolume raw{bytes};
    const std::uint32_t hello = raw.Load32(raw.EntryOffset(RootNode, "hello.txt"));
    raw.Store16(raw.InodeOffset(hello) + 26, 31999);
    const std::uint32_t a = raw.Load32(raw.EntryOffset(RootNode, "a"));
    raw.Store16(raw.InodeOffset(a) + 26, 32000);
    const std::unique_ptr<MemoryDevice> limited = DeviceFrom(bytes);
    MountedFilesystem ext2(Ext2Type(), *limited);
    Succeeds(ext2->Link(hello, ext2->Root(), "link-32000"));
    REQUIRE(ext2->Link(hello, ext2->Root(), "link-32001").error() == Error::TooManyLinks);
    REQUIRE(ErrorOf(ext2->MakeDirectory(a, "one-more", 0755)) == Error::TooManyLinks);
    REQUIRE(ext2->Rename(ext2->Root(), "lost+found", a, "lost+found").error() ==
            Error::TooManyLinks);
#endif
}

#ifdef FEATHERDISK_EXT_IMAGES
namespace
{

// Ignores every failure: after the cut, everything that writes fails.
void TreeScenario(Filesystem& ext2)
{
    const NodeId root = ext2.Root();
    const Result<NodeId> directory = ext2.MakeDirectory(root, "crash-dir", 0755);
    if (!directory)
    {
        return;
    }
    for (int number = 0; number < 4; ++number)
    {
        if (const Result<NodeId> made =
                ext2.Create(*directory, NumberedName("file", number, 40), 0644))
        {
            static_cast<void>(ext2.Write(*made, 0, FeatherDisk::Tests::BytesOfText(Pattern(1500))));
        }
    }
    const Result<NodeId> sub = ext2.MakeDirectory(*directory, "sub", 0755);
    static_cast<void>(ext2.SymbolicLink(*directory, "fast", "../hello.txt"));
    static_cast<void>(ext2.SymbolicLink(*directory, "slow", std::string(300, 's')));
    if (const Result<NodeId> hello = Walk(ext2, "hello.txt"))
    {
        static_cast<void>(ext2.Link(*hello, *directory, "hello-link"));
    }
    // Within one directory, across directories, and over an existing file.
    static_cast<void>(ext2.Rename(*directory, NumberedName("file", 0, 40), *directory,
                                  NumberedName("renamed", 0, 90)));
    const Result<NodeId> b = Walk(ext2, "a/b");
    if (!b)
    {
        return;
    }
    static_cast<void>(ext2.Rename(*directory, NumberedName("file", 1, 40), *b, "moved.txt"));
    if (const Result<NodeId> c = Walk(ext2, "a/b/c"))
    {
        static_cast<void>(ext2.Rename(*directory, NumberedName("file", 2, 40), *c, "deep.txt"));
    }
    // A directory to a new parent, then over an empty directory under another.
    if (sub)
    {
        static_cast<void>(ext2.MakeDirectory(*sub, "child", 0755));
        static_cast<void>(ext2.Rename(*directory, "sub", *b, "sub"));
    }
    static_cast<void>(ext2.MakeDirectory(root, "landing", 0755));
    static_cast<void>(ext2.Rename(*b, "sub", root, "landing"));
    static_cast<void>(ext2.Remove(*directory, "hello-link"));
    static_cast<void>(ext2.Remove(*directory, "slow"));
    if (const Result<NodeId> landing = Walk(ext2, "landing"))
    {
        static_cast<void>(ext2.RemoveDirectory(*landing, "child"));
    }
    static_cast<void>(ext2.RemoveDirectory(root, "landing"));
}

} // namespace
#endif

TEST_CASE("an ext2 volume stopped during directory changes needs only e2fsck's routine repairs")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    if (!HaveE2fsck())
    {
        SKIP("e2fsck is not installed");
    }
    for (const std::string_view variant : {"1k", "ext4-csum64"})
    {
        INFO(variant);
        const std::vector<std::uint8_t> original = LoadExtImage(variant);
        std::uint64_t total = 0;
        {
            const std::unique_ptr<MemoryDevice> device = DeviceFrom(original);
            {
                MountedFilesystem ext2(Ext2Type(), *device);
                TreeScenario(*ext2);
                REQUIRE(ErrorOf(Walk(*ext2, "landing")) == Error::NotFound);
                REQUIRE(Value(Walk(*ext2, "a/b/moved.txt")) != 0);
            }
            total = device->write_count;
            CheckClean(*device, "tree-crash-complete");
        }
        REQUIRE(total > 50);
        for (std::uint64_t cut = 1; cut < total; ++cut)
        {
            INFO("stopped after write " << cut << " of " << total);
            const std::unique_ptr<MemoryDevice> device = DeviceFrom(original);
            device->FailWritesAfter(cut);
            const Result<Filesystem*> mounted = Ext2Type().Mount(*device, {.read_only = false});
            REQUIRE(mounted.has_value());
            TreeScenario(**mounted);
            const std::unique_ptr<MemoryDevice> stopped = CopyOf(*device);
            device->FailWritesAfter(UINT64_MAX);
            Succeeds(Ext2Type().Unmount(**mounted));
            const ToolRun report = RunE2fsck(*stopped, "tree-crash");
            INFO(report.output);
            REQUIRE(OnlyRoutineRepairs(report.output, true));
        }
    }
#endif
}
