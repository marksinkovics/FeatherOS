// SPDX-License-Identifier: MIT OR Apache-2.0
// ext2 writes over images that mke2fs made, each change checked by e2fsck -fn and, where it
// helps, read back by debugfs: implementations independent of the core.
#include <catch2/catch_test_macros.hpp>

#include "fs/ext2.h"
#include "tests/ext2_image.h"
#include "tests/filesystem_helpers.h"
#include "tests/memory_device.h"
#include "tests/page_cache_support.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifdef FEATHERDISK_EXT_IMAGES
namespace
{

using FeatherDisk::AttributeChanges;
using FeatherDisk::Attributes;
using FeatherDisk::Error;
using FeatherDisk::Filesystem;
using FeatherDisk::NodeId;
using FeatherDisk::NodeType;
using FeatherDisk::Result;
using FeatherDisk::Status;
using FeatherDisk::Timestamp;
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
using FeatherDisk::Tests::Sorted;
using FeatherDisk::Tests::Succeeds;
using FeatherDisk::Tests::ToolRun;
using FeatherDisk::Tests::Value;
using FeatherDisk::Tests::Walk;
using FeatherDisk::Tests::WriteAt;
using FeatherDisk::Tests::WriteNewFile;

// Superblock fields, from the start of the volume.
constexpr std::uint64_t StateOffset = 1024 + 58;
constexpr std::uint64_t MountCountOffset = 1024 + 52;
constexpr std::uint64_t CompatibleOffset = 1024 + 92;
constexpr std::uint64_t ReadOnlyCompatibleOffset = 1024 + 100;
constexpr std::uint32_t LargeFileFeature = 0x0002;
constexpr std::uint64_t GiB = std::uint64_t{1} << 30;

std::uint64_t FreeInodes(Filesystem& filesystem)
{
    return Value(filesystem.Statistics()).free_nodes;
}

std::uint16_t Field16(MemoryDevice& device, std::uint64_t offset)
{
    const std::span<std::uint8_t> sector = device.Sector(offset / device.SectorSize());
    const std::size_t within = offset % device.SectorSize();
    return static_cast<std::uint16_t>(sector[within] | (sector[within + 1] << 8));
}

std::uint32_t Field32(MemoryDevice& device, std::uint64_t offset)
{
    return Field16(device, offset) | (std::uint32_t{Field16(device, offset + 2)} << 16);
}

// The blocks a file of size bytes without holes takes: data, and the indirect blocks over it.
std::uint64_t DenseBlocks(std::uint64_t size, std::uint64_t block_size)
{
    const std::uint64_t per_block = block_size / 4;
    const std::uint64_t data = (size + block_size - 1) / block_size;
    std::uint64_t blocks = data;
    if (data > 12)
    {
        ++blocks;
    }
    if (data > 12 + per_block)
    {
        const std::uint64_t past = data - 12 - per_block;
        blocks += 1 + (past + per_block - 1) / per_block;
    }
    return blocks;
}

std::int64_t Seconds()
{
    return static_cast<std::int64_t>(std::time(nullptr));
}

void CheckDebugfsContents(MemoryDevice& device, std::string_view name, std::string_view path,
                          const std::string& expected)
{
    const ToolRun run = RunDebugfs(device, name, "cat " + std::string(path));
    if (run.ran)
    {
        REQUIRE(run.output == expected);
    }
}

void CreateWriteAndRead(std::string_view variant)
{
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(variant));
    const std::string text = "Written by the FeatherDisk core.\n";
    NodeId file = 0;
    {
        MountedFilesystem ext2(Ext2Type(), *device);
        REQUIRE(ext2->GetCapabilities().writable);
        const NodeId root = ext2->Root();
        const std::uint64_t free_inodes = FreeInodes(*ext2);
        const std::int64_t before = Seconds();
        file = Value(ext2->Create(root, "new.txt", 0100640));
        REQUIRE(FreeInodes(*ext2) == free_inodes - 1);
        const Attributes created = Value(ext2->GetAttributes(file));
        REQUIRE(created.type == NodeType::File);
        REQUIRE(created.mode == 0640);
        REQUIRE(created.owner == 0);
        REQUIRE(created.group == 0);
        REQUIRE(created.link_count == 1);
        REQUIRE(created.size == 0);
        REQUIRE(created.modify_time.seconds >= before);
        REQUIRE(created.modify_time.seconds <= Seconds());
        REQUIRE(created.change_time.seconds == created.modify_time.seconds);
        REQUIRE(Value(ext2->Lookup(root, "new.txt")) == file);

        REQUIRE(WriteAt(*ext2, file, 0, text) == text.size());
        REQUIRE(ReadAll(*ext2, file) == text);
        // Over the middle, and appended.
        REQUIRE(WriteAt(*ext2, file, 8, "BY") == 2);
        REQUIRE(WriteAt(*ext2, file, text.size(), "more\n") == 5);
        REQUIRE(ReadAll(*ext2, file) == "Written BY the FeatherDisk core.\nmore\n");
        const std::vector<std::string> names = List(*ext2, root);
        REQUIRE(std::count(names.begin(), names.end(), "new.txt") == 1);

        REQUIRE(ErrorOf(ext2->Create(root, "new.txt", 0644)) == Error::Exists);
        REQUIRE(ErrorOf(ext2->Create(root, "hello.txt", 0644)) == Error::Exists);
        REQUIRE(ErrorOf(ext2->Create(root, ".", 0644)) == Error::Exists);
        REQUIRE(ErrorOf(ext2->Create(root, "..", 0644)) == Error::Exists);
        REQUIRE(ErrorOf(ext2->Create(root, "a/b", 0644)) == Error::InvalidArgument);
        REQUIRE(ErrorOf(ext2->Create(root, "", 0644)) == Error::InvalidArgument);
        REQUIRE(ErrorOf(ext2->Create(root, std::string(256, 'x'), 0644)) == Error::NameTooLong);
        REQUIRE(ErrorOf(ext2->Create(file, "inner", 0644)) == Error::NotDirectory);
        const NodeId directory = Value(Walk(*ext2, "a/b"));
        REQUIRE(ErrorOf(ext2->Write(directory, 0, BytesOfText("x"))) == Error::IsDirectory);
        REQUIRE(ErrorOf(ext2->Write(Value(Walk(*ext2, "pipe")), 0, BytesOfText("x"))) ==
                Error::InvalidArgument);
        REQUIRE(ErrorOf(ext2->Write(Value(Walk(*ext2, "fast-link")), 0, BytesOfText("x"))) ==
                Error::InvalidArgument);
        // The resize inode and the other reserved inodes are not files to change.
        REQUIRE(ErrorOf(ext2->Write(7, 0, BytesOfText("x"))) == Error::InvalidArgument);
        REQUIRE(ext2->Truncate(7, 0).error() == Error::InvalidArgument);

        // A 255-byte name, in a nested directory.
        const std::string longest(255, 'n');
        const NodeId deep = Value(ext2->Create(Value(Walk(*ext2, "a/b/c")), longest, 0600));
        REQUIRE(WriteAt(*ext2, deep, 0, "deep and long\n") == 14);
        REQUIRE(Value(Walk(*ext2, "a/b/c/" + longest)) == deep);
    }
    {
        MountedFilesystem ext2(Ext2Type(), *device, true);
        REQUIRE(ReadAll(*ext2, Value(Walk(*ext2, "new.txt"))) ==
                "Written BY the FeatherDisk core.\nmore\n");
        REQUIRE(ReadAll(*ext2, Value(Walk(*ext2, "hello.txt"))) == "Hello from ext2.\n");
        const std::map<std::string, NodeType> types = [&]
        {
            std::map<std::string, NodeType> found;
            std::uint64_t cookie = 0;
            FeatherDisk::DirectoryEntry entry{};
            while (Value(ext2->ReadDirectory(ext2->Root(), cookie, entry)))
            {
                found[std::string(entry.name.data(), entry.name_length)] = entry.type;
            }
            return found;
        }();
        REQUIRE(types.at("new.txt") == NodeType::File);
    }
    CheckClean(*device, std::string(variant) + "-create");
    CheckDebugfsContents(*device, variant, "/new.txt", "Written BY the FeatherDisk core.\nmore\n");
}

} // namespace
#endif

TEST_CASE("ext2 mounts writable and marks the volume not clean until it syncs")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage("1k"));
    const std::uint16_t mounts = Field16(*device, MountCountOffset);
    REQUIRE(Field16(*device, StateOffset) == 1);
    {
        MountedFilesystem ext2(Ext2Type(), *device);
        REQUIRE(ext2->GetCapabilities().writable);
        REQUIRE(Field16(*device, StateOffset) == 0);
        REQUIRE(Field16(*device, MountCountOffset) == mounts + 1);
        REQUIRE(Field32(*device, 1024 + 44) >= static_cast<std::uint32_t>(Seconds() - 5));
        Succeeds(ext2->Sync());
        REQUIRE(Field16(*device, StateOffset) == 1);
        const std::uint32_t flushes = device->flush_count;
        // The first change after a sync marks the volume again, before it changes anything.
        WriteNewFile(*ext2, "after-sync.txt", "x");
        REQUIRE(Field16(*device, StateOffset) == 0);
        REQUIRE(device->flush_count > flushes);
        Succeeds(ext2->Sync());
        REQUIRE(Field16(*device, StateOffset) == 1);
    }
    REQUIRE(Field16(*device, StateOffset) == 1);
    CheckClean(*device, "mount-state");

    // Mounting for writing and unmounting changes only the superblock.
    const std::unique_ptr<MemoryDevice> untouched = DeviceFrom(LoadExtImage("4k"));
    {
        MountedFilesystem ext2(Ext2Type(), *untouched);
        REQUIRE(ext2->GetCapabilities().writable);
    }
    CheckClean(*untouched, "mount-unmount");
#endif
}

TEST_CASE("ext2 mounts read-only when a volume must not be written")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    const auto writable = [](std::string_view variant, auto damage)
    {
        std::vector<std::uint8_t> bytes = LoadExtImage(variant);
        RawVolume raw{bytes};
        damage(raw);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
        bool result = false;
        {
            MountedFilesystem ext2(Ext2Type(), *device);
            result = ext2->GetCapabilities().writable;
            if (!result)
            {
                REQUIRE(ErrorOf(ext2->Create(ext2->Root(), "x", 0644)) == Error::ReadOnly);
                REQUIRE(ext2->Remove(ext2->Root(), "hello.txt").error() == Error::ReadOnly);
            }
        }
        if (!result)
        {
            REQUIRE(device->write_count == 0);
        }
        return result;
    };
    const auto unchanged = [](RawVolume&) {};
    REQUIRE(writable("1k", unchanged));
    REQUIRE(writable("4k", unchanged));
    REQUIRE(writable("nofiletype", unchanged));
    REQUIRE(writable("rev0", unchanged));
    // Not cleanly unmounted, errors recorded, both.
    REQUIRE_FALSE(writable("1k", [](RawVolume& raw) { raw.Store16(StateOffset, 0); }));
    REQUIRE_FALSE(writable("1k", [](RawVolume& raw) { raw.Store16(StateOffset, 3); }));
    REQUIRE_FALSE(writable("1k", [](RawVolume& raw) { raw.Store16(StateOffset, 2); }));
    // An ext3 journal is written (ext_transaction_tests.cpp); an unknown read-only compatible
    // feature and an unknown compatible one are not.
    REQUIRE(writable("ext3", unchanged));
    REQUIRE_FALSE(writable("1k", [](RawVolume& raw)
                           { raw.Store32(ReadOnlyCompatibleOffset, 0x80000000U | 3U); }));
    REQUIRE_FALSE(
        writable("1k", [](RawVolume& raw)
                 { raw.Store32(CompatibleOffset, raw.Load32(CompatibleOffset) | 0x40U); }));
    // A first inode among the reserved ones.
    REQUIRE_FALSE(writable("1k", [](RawVolume& raw) { raw.Store32(1024 + 84, 3); }));
    // Group descriptors counting more free blocks than a group holds.
    REQUIRE_FALSE(
        writable("1k", [](RawVolume& raw) { raw.Store16(raw.DescriptorOffset() + 12, 0xFFFF); }));

    // A device that refuses writes.
    const std::unique_ptr<MemoryDevice> locked = DeviceFrom(LoadExtImage("1k"));
    locked->read_only = true;
    {
        MountedFilesystem ext2(Ext2Type(), *locked);
        REQUIRE_FALSE(ext2->GetCapabilities().writable);
        REQUIRE(ReadAll(*ext2, Value(Walk(*ext2, "hello.txt"))) == "Hello from ext2.\n");
    }
#endif
}

TEST_CASE("ext2 creates, writes and reads files with 1 KiB blocks")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    CreateWriteAndRead("1k");
#endif
}

TEST_CASE("ext2 creates, writes and reads files with 4 KiB blocks and large inodes")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    CreateWriteAndRead("4k");
#endif
}

TEST_CASE("ext2 creates files without the filetype feature and on revision 0")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    CreateWriteAndRead("nofiletype");
    CreateWriteAndRead("rev0");
#endif
}

TEST_CASE("ext4 layouts without a journal or extents take files with every checksum kept")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    // metadata_csum with its seed, 64bit descriptors with metadata_csum, and gdt_csum.
    CreateWriteAndRead("ext4-csum");
    CreateWriteAndRead("ext4-csum64");
    CreateWriteAndRead("ext4-gdtcsum");
    // Extents, and extents beside the image's block maps.
    CreateWriteAndRead("ext4-nojournal");
    CreateWriteAndRead("ext4-mixed");
#endif
}

TEST_CASE("ext3 and ext4 with a journal take files through transactions")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    // Indirect blocks, extents, 64bit, the three journal checksum versions and 4 KiB blocks.
    for (const std::string_view variant : {"ext3", "ext4", "ext4-blockmap", "ext4-64bit",
                                           "ext4-extents", "ext4-journal-gdtcsum", "ext4-4k"})
    {
        CreateWriteAndRead(variant);
    }
#endif
}

TEST_CASE("ext2 writes through direct, single and double indirect blocks")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const std::string_view variant : {"1k", "4k", "ext4-csum", "ext4-csum64", "ext4-gdtcsum",
                                           "ext3", "ext4-blockmap", "ext4-64bit"})
    {
        INFO(variant);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(variant));
        // 300 KiB: past the 268 KiB that direct and single indirect blocks reach at 1 KiB.
        const std::string text = Pattern(300 * 1024);
        {
            MountedFilesystem ext2(Ext2Type(), *device);
            const std::uint32_t block_size = Value(ext2->Statistics()).block_size;
            const std::uint32_t free = FreeBlocks(*ext2);
            const NodeId file = Value(ext2->Create(ext2->Root(), "big-new.bin", 0644));
            // Unaligned pieces, so most of them start and end inside a block.
            for (std::size_t offset = 0; offset < text.size(); offset += 7001)
            {
                const std::string piece = text.substr(offset, 7001);
                REQUIRE(WriteAt(*ext2, file, offset, piece) == piece.size());
            }
            REQUIRE(ReadAll(*ext2, file) == text);
            REQUIRE(FreeBlocks(*ext2) == free - DenseBlocks(text.size(), block_size));
            REQUIRE(ReadAt(*ext2, file, 268 * 1024 - 10, 30) == text.substr(268 * 1024 - 10, 30));
        }
        CheckClean(*device, std::string(variant) + "-indirect");
        CheckDebugfsContents(*device, variant, "/big-new.bin", text);
        MountedFilesystem ext2(Ext2Type(), *device, true);
        REQUIRE(ReadAll(*ext2, Value(Walk(*ext2, "big-new.bin"))) == text);
    }
#endif
}

TEST_CASE("ext2 leaves holes when writing past the end of a file")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage("1k"));
    {
        MountedFilesystem ext2(Ext2Type(), *device);
        const std::uint32_t free = FreeBlocks(*ext2);
        const NodeId file = Value(ext2->Create(ext2->Root(), "sparse-new.bin", 0644));
        REQUIRE(WriteAt(*ext2, file, 0, "head") == 4);
        // At 5 MiB, on a block boundary: one data block, a double and a single indirect one.
        constexpr std::uint64_t Tail = 5 * 1024 * 1024;
        REQUIRE(WriteAt(*ext2, file, Tail, "tail") == 4);
        REQUIRE(FreeBlocks(*ext2) == free - 4);
        REQUIRE(Value(ext2->GetAttributes(file)).size == Tail + 4);
        REQUIRE(ReadAt(*ext2, file, 0, 8) == std::string("head") + std::string(4, '\0'));
        REQUIRE(ReadAt(*ext2, file, Tail - 3, 7) == std::string(3, '\0') + "tail");
        REQUIRE(ReadAt(*ext2, file, 1024 * 1024, 4096) == std::string(4096, '\0'));
        // Inside the hole, not on a boundary: one data block and a single indirect one.
        REQUIRE(WriteAt(*ext2, file, 3 * 1024 * 1024 + 100, "middle") == 6);
        REQUIRE(FreeBlocks(*ext2) == free - 6);
        REQUIRE(ReadAt(*ext2, file, 3 * 1024 * 1024 + 90, 20) ==
                std::string(10, '\0') + "middle" + std::string(4, '\0'));

        // Bytes left past the end of a file must read as zeros once the file covers them.
        const NodeId small = Value(ext2->Create(ext2->Root(), "tail-zeros.txt", 0644));
        REQUIRE(WriteAt(*ext2, small, 0, std::string(100, 'x')) == 100);
        std::vector<std::uint8_t> bytes = BytesOf(*device);
        RawVolume raw{bytes};
        const std::uint32_t block = raw.InodeBlock(static_cast<std::uint32_t>(small), 0);
        for (std::uint64_t offset = 100; offset < 1024; ++offset)
        {
            const std::uint64_t byte = raw.BlockOffset(block) + offset;
            device->Sector(byte / 512)[byte % 512] = 0xEE;
        }
        // The cache may hold the block's old bytes, so the next mount reads the change.
    }
    {
        MountedFilesystem ext2(Ext2Type(), *device);
        const NodeId small = Value(Walk(*ext2, "tail-zeros.txt"));
        REQUIRE(WriteAt(*ext2, small, 1500, "end") == 3);
        REQUIRE(ReadAll(*ext2, small) == std::string(100, 'x') + std::string(1400, '\0') + "end");
    }
    CheckClean(*device, "holes");
#endif
}

TEST_CASE("ext2 truncates across indirect boundaries and grows files with holes")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage("1k"));
    const std::string text = Pattern(300 * 1024);
    std::uint32_t free = 0;
    {
        MountedFilesystem ext2(Ext2Type(), *device);
        free = FreeBlocks(*ext2);
        const NodeId file = Value(ext2->Create(ext2->Root(), "shrinking.bin", 0644));
        REQUIRE(WriteAt(*ext2, file, 0, text) == text.size());
        // Inside the double indirect range, at its start, inside the single indirect range, at
        // its start, inside a direct block, and to nothing.
        for (const std::uint64_t size : {std::uint64_t{280 * 1024 + 17}, std::uint64_t{268 * 1024},
                                         std::uint64_t{200 * 1024 + 5}, std::uint64_t{12 * 1024},
                                         std::uint64_t{5000}, std::uint64_t{0}})
        {
            INFO("size " << size);
            Succeeds(ext2->Truncate(file, size));
            REQUIRE(Value(ext2->GetAttributes(file)).size == size);
            REQUIRE(ReadAll(*ext2, file) == text.substr(0, size));
            REQUIRE(FreeBlocks(*ext2) == free - DenseBlocks(size, 1024));
        }
    }
    CheckClean(*device, "truncate-shrink");
    {
        MountedFilesystem ext2(Ext2Type(), *device);
        const NodeId file = Value(Walk(*ext2, "shrinking.bin"));
        REQUIRE(WriteAt(*ext2, file, 0, text.substr(0, 5000)) == 5000);
        Succeeds(ext2->Truncate(file, 3000));
        // Growing allocates nothing, and what was cut off reads as zeros.
        const std::uint32_t before = FreeBlocks(*ext2);
        Succeeds(ext2->Truncate(file, 1024 * 1024));
        REQUIRE(FreeBlocks(*ext2) == before);
        REQUIRE(Value(ext2->GetAttributes(file)).size == 1024 * 1024);
        REQUIRE(ReadAll(*ext2, file) ==
                text.substr(0, 3000) + std::string(1024 * 1024 - 3000, '\0'));
        Succeeds(ext2->Truncate(file, 1024 * 1024));
        REQUIRE(ErrorOf(ext2->Truncate(Value(Walk(*ext2, "a")), 0)) == Error::IsDirectory);
        REQUIRE(ext2->Truncate(Value(Walk(*ext2, "pipe")), 0).error() == Error::InvalidArgument);
        REQUIRE(ext2->Truncate(file, std::uint64_t{1} << 50).error() == Error::FileTooLarge);
    }
    CheckClean(*device, "truncate-grow");
    {
        // The image's own files: big.txt through its double indirect block, and sparse.bin's
        // triple indirect tail.
        MountedFilesystem ext2(Ext2Type(), *device);
        const NodeId big = Value(Walk(*ext2, "big.txt"));
        const std::string big_text = ReadAll(*ext2, big);
        Succeeds(ext2->Truncate(big, 1000));
        REQUIRE(ReadAll(*ext2, big) == big_text.substr(0, 1000));
        const NodeId sparse = Value(Walk(*ext2, "sparse.bin"));
        Succeeds(ext2->Truncate(sparse, 300002));
        REQUIRE(ReadAt(*ext2, sparse, 300000, 10) == "mi");
        Succeeds(ext2->Truncate(sparse, 0));
    }
    CheckClean(*device, "truncate-image-files");
#endif
}

TEST_CASE("ext2 removes files, hard links, symbolic links and special nodes")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    // many/ has a hash index on the ext4-csum volumes.
    for (const std::string_view variant : {"1k", "4k", "ext4-csum", "ext4-csum64", "ext4-gdtcsum",
                                           "ext3", "ext4-blockmap", "ext4-64bit", "ext4-4k"})
    {
        INFO(variant);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(variant));
        {
            MountedFilesystem ext2(Ext2Type(), *device);
            const NodeId root = ext2->Root();
            const std::uint32_t free_blocks = FreeBlocks(*ext2);
            const std::uint64_t free_inodes = FreeInodes(*ext2);
            WriteNewFile(*ext2, "gone.bin", Pattern(100 * 1024));
            Succeeds(ext2->Remove(root, "gone.bin"));
            REQUIRE(FreeBlocks(*ext2) == free_blocks);
            REQUIRE(FreeInodes(*ext2) == free_inodes);
            REQUIRE(ErrorOf(ext2->Lookup(root, "gone.bin")) == Error::NotFound);

            // hello.txt and hard.txt share an inode, which stays until the second goes.
            const NodeId hello = Value(Walk(*ext2, "hello.txt"));
            Succeeds(ext2->Remove(root, "hello.txt"));
            REQUIRE(Value(ext2->GetAttributes(hello)).link_count == 1);
            REQUIRE(ReadAll(*ext2, Value(Walk(*ext2, "hard.txt"))) == "Hello from ext2.\n");
            REQUIRE(FreeInodes(*ext2) == free_inodes);
            Succeeds(ext2->Remove(root, "hard.txt"));
            REQUIRE(FreeInodes(*ext2) == free_inodes + 1);
            REQUIRE(ErrorOf(ext2->GetAttributes(hello)) == Error::NotFound);

            for (const char* name :
                 {"fast-link", "slow-link", "pipe", "tool", "empty.txt", "big.txt", "sparse.bin"})
            {
                INFO(name);
                Succeeds(ext2->Remove(root, name));
                REQUIRE(ErrorOf(ext2->Lookup(root, name)) == Error::NotFound);
            }
            REQUIRE(ext2->Remove(root, "a").error() == Error::IsDirectory);
            REQUIRE(ext2->Remove(root, ".").error() == Error::IsDirectory);
            REQUIRE(ext2->Remove(root, "..").error() == Error::IsDirectory);
            REQUIRE(ext2->Remove(root, "missing").error() == Error::NotFound);
            REQUIRE(ext2->Remove(Value(Walk(*ext2, "many/entry-with-a-longer-name-000.txt")), "x")
                        .error() == Error::NotDirectory);
            REQUIRE(Sorted(List(*ext2, root)) ==
                    std::vector<std::string>{"a/", "lost+found/", "many/"});

            // Every record of a directory over several blocks, so first records of blocks go
            // too, then new files in the space they left.
            const NodeId many = Value(Walk(*ext2, "many"));
            const std::uint64_t many_size = Value(ext2->GetAttributes(many)).size;
            for (int number = 0; number < 300; ++number)
            {
                std::array<char, 64> name{};
                std::snprintf(name.data(), name.size(), "entry-with-a-longer-name-%03d.txt",
                              number);
                Succeeds(ext2->Remove(many, name.data()));
            }
            REQUIRE(List(*ext2, many).empty());
            for (int number = 0; number < 40; ++number)
            {
                WriteNewFile(*ext2, "many/" + NumberedName("again", number, 30), "again\n");
            }
            REQUIRE(List(*ext2, many).size() == 40);
            REQUIRE(Value(ext2->GetAttributes(many)).size == many_size);
        }
        CheckClean(*device, std::string(variant) + "-remove");
    }
#endif
}

TEST_CASE("ext2 removes a file's extended attribute block with its last link")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage("1k"));
    const ToolRun set =
        ChangeWithDebugfs(*device, "xattr", "ea_set /hello.txt user.featherdisk some-value");
    if (!set.ran)
    {
        SKIP("debugfs is not installed");
    }
    std::vector<std::uint8_t> bytes = BytesOf(*device);
    RawVolume raw{bytes};
    const std::uint32_t hello = raw.Load32(raw.EntryOffset(2, "hello.txt"));
    // 128-byte inodes have no room inside, so the attribute has a block of its own.
    REQUIRE(raw.Load32(raw.InodeOffset(hello) + 104) != 0);
    {
        MountedFilesystem ext2(Ext2Type(), *device);
        const std::uint32_t free = FreeBlocks(*ext2);
        Succeeds(ext2->Remove(ext2->Root(), "hello.txt"));
        REQUIRE(FreeBlocks(*ext2) == free);
        Succeeds(ext2->Remove(ext2->Root(), "hard.txt"));
        // The data block and the attribute block.
        REQUIRE(FreeBlocks(*ext2) == free + 2);
    }
    CheckClean(*device, "xattr");
#endif
}

TEST_CASE("ext2 grows a directory past its direct blocks and shrinks it back")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const std::string_view variant : {"1k", "ext4-csum", "ext3", "ext4-blockmap"})
    {
        INFO(variant);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(variant));
        std::vector<std::string> names;
        {
            MountedFilesystem ext2(Ext2Type(), *device);
            const NodeId root = ext2->Root();
            // Four 200-byte names fill a 1 KiB block, so 100 files need 25 blocks: an indirect one.
            for (int number = 0; number < 100; ++number)
            {
                names.push_back(NumberedName("grown", number, 200));
                const NodeId file = Value(ext2->Create(root, names.back(), 0644));
                REQUIRE(WriteAt(*ext2, file, 0, names.back()) == names.back().size());
            }
            REQUIRE(Value(ext2->GetAttributes(root)).size > 13 * 1024);
            REQUIRE(List(*ext2, root).size() == 112);
            for (const std::string& name : names)
            {
                REQUIRE(ReadAll(*ext2, Value(ext2->Lookup(root, name))) == name);
            }
        }
        CheckClean(*device, "directory-grown");
        {
            MountedFilesystem ext2(Ext2Type(), *device);
            for (const std::string& name : names)
            {
                Succeeds(ext2->Remove(ext2->Root(), name));
            }
            REQUIRE(List(*ext2, ext2->Root()).size() == 12);
        }
        CheckClean(*device, "directory-emptied");
    }
#endif
}

TEST_CASE("ext2 fills the volume, then frees and reuses the space")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    // ext4-csum's groups 2 and 3 start uninitialised; ext4-nojournal's file takes extents.
    for (const std::string_view variant : {"1k", "ext4-csum", "ext4-nojournal", "ext3", "ext4"})
    {
        INFO(variant);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(variant));
        std::uint64_t written = 0;
        {
            MountedFilesystem ext2(Ext2Type(), *device);
            const NodeId file = Value(ext2->Create(ext2->Root(), "filler.bin", 0644));
            const std::string chunk = Pattern(64 * 1024);
            while (true)
            {
                const Result<std::size_t> count = ext2->Write(file, written, BytesOfText(chunk));
                if (!count)
                {
                    REQUIRE(count.error() == Error::NoSpace);
                    break;
                }
                written += *count;
                REQUIRE(*count > 0);
                if (*count < chunk.size())
                {
                    REQUIRE(ErrorOf(ext2->Write(file, written, BytesOfText("x"))) ==
                            Error::NoSpace);
                    break;
                }
            }
            REQUIRE(FreeBlocks(*ext2) == 0);
            REQUIRE(Value(ext2->GetAttributes(file)).size == written);
            // Writing where blocks already are still works; a new empty file still fits, as long
            // as the root directory has room.
            REQUIRE(WriteAt(*ext2, file, 10, "rewritten") == 9);
            const NodeId empty = Value(ext2->Create(ext2->Root(), "empty-on-full.txt", 0644));
            REQUIRE(ErrorOf(ext2->Write(empty, 0, BytesOfText("x"))) == Error::NoSpace);
            Succeeds(ext2->Truncate(empty, 4096));
        }
        CheckClean(*device, "full");
        {
            MountedFilesystem ext2(Ext2Type(), *device);
            const NodeId file = Value(Walk(*ext2, "filler.bin"));
            std::string expected = Pattern(64 * 1024);
            expected.replace(10, 9, "rewritten");
            REQUIRE(ReadAt(*ext2, file, 0, expected.size()) == expected);
            Succeeds(ext2->Remove(ext2->Root(), "filler.bin"));
            REQUIRE(FreeBlocks(*ext2) >= written / 1024);
            WriteNewFile(*ext2, "reused.bin", Pattern(200 * 1024));
            REQUIRE(ReadAll(*ext2, Value(Walk(*ext2, "reused.bin"))) == Pattern(200 * 1024));
        }
        CheckClean(*device, "full-reused");
    }

    // And the inodes: empty files until none is left, through ext4-gdtcsum's uninitialised
    // inode tables too.
    for (const std::string_view variant : {"4k", "ext4-gdtcsum"})
    {
        INFO(variant);
        const std::unique_ptr<MemoryDevice> inodes = DeviceFrom(LoadExtImage(variant));
        {
            MountedFilesystem ext2(Ext2Type(), *inodes);
            const NodeId directory = Value(Walk(*ext2, "a/b"));
            int created = 0;
            while (true)
            {
                const Result<NodeId> file =
                    ext2->Create(directory, NumberedName("node", created, 12), 0644);
                if (!file)
                {
                    REQUIRE(file.error() == Error::NoSpace);
                    break;
                }
                ++created;
            }
            REQUIRE(FreeInodes(*ext2) == 0);
            REQUIRE(created > 100);
            Succeeds(ext2->Remove(directory, NumberedName("node", 7, 12)));
            Value(ext2->Create(directory, "one-more", 0644));
        }
        CheckClean(*inodes, "no-inodes");
    }
#endif
}

TEST_CASE("ext2 sets modes, owners and times")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const std::string_view variant :
         {"1k", "4k", "ext4-csum", "ext4-gdtcsum", "ext4-nojournal", "ext3", "ext4"})
    {
        INFO(variant);
        const bool nanoseconds = variant != "1k";
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(variant));
        {
            MountedFilesystem ext2(Ext2Type(), *device);
            const NodeId hello = Value(Walk(*ext2, "hello.txt"));
            AttributeChanges changes{};
            changes.mode = 0104711;
            changes.owner = 70000;
            changes.group = 4243;
            changes.access_time = Timestamp{.seconds = 1700000000, .nanoseconds = 123456789};
            changes.modify_time = Timestamp{.seconds = 1600000000, .nanoseconds = 5};
            Succeeds(ext2->SetAttributes(hello, changes));
            AttributeChanges root_mode{};
            root_mode.mode = 0700;
            Succeeds(ext2->SetAttributes(ext2->Root(), root_mode));
            Succeeds(ext2->SetAttributes(hello, AttributeChanges{}));
            AttributeChanges bad{};
            bad.modify_time = Timestamp{.seconds = 0, .nanoseconds = 1'000'000'000};
            REQUIRE(ext2->SetAttributes(hello, bad).error() == Error::InvalidArgument);
            REQUIRE(ext2->SetAttributes(1, changes).error() == Error::InvalidArgument);
        }
        {
            MountedFilesystem ext2(Ext2Type(), *device, true);
            const Attributes hello = Value(ext2->GetAttributes(Value(Walk(*ext2, "hello.txt"))));
            REQUIRE(hello.type == NodeType::File);
            REQUIRE(hello.mode == 04711);
            REQUIRE(hello.owner == 70000);
            REQUIRE(hello.group == 4243);
            REQUIRE(hello.access_time.seconds == 1700000000);
            REQUIRE(hello.access_time.nanoseconds == (nanoseconds ? 123456789U : 0U));
            REQUIRE(hello.modify_time.seconds == 1600000000);
            REQUIRE(hello.modify_time.nanoseconds == (nanoseconds ? 5U : 0U));
            REQUIRE(hello.change_time.seconds >= Seconds() - 5);
            REQUIRE(hello.size == 17);
            const Attributes root = Value(ext2->GetAttributes(ext2->Root()));
            REQUIRE(root.type == NodeType::Directory);
            REQUIRE(root.mode == 0700);
        }
        CheckClean(*device, std::string(variant) + "-attributes");
        const ToolRun stat = RunDebugfs(*device, variant, "stat /hello.txt");
        if (stat.ran)
        {
            REQUIRE(stat.output.find("Mode:  04711") != std::string::npos);
            REQUIRE(stat.output.find("User: 70000") != std::string::npos);
        }
    }
#endif
}

TEST_CASE("ext2 sets large_file with the first file past 2 GiB")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    std::vector<std::uint8_t> bytes = LoadExtImage("1k");
    RawVolume raw{bytes};
    raw.Store32(ReadOnlyCompatibleOffset, raw.Load32(ReadOnlyCompatibleOffset) & ~LargeFileFeature);
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
    {
        MountedFilesystem ext2(Ext2Type(), *device);
        REQUIRE(ext2->GetCapabilities().maximum_file_size > 3 * GiB);
        const NodeId file = Value(ext2->Create(ext2->Root(), "large.bin", 0644));
        REQUIRE(WriteAt(*ext2, file, 3 * GiB, "far") == 3);
        REQUIRE((Field32(*device, ReadOnlyCompatibleOffset) & LargeFileFeature) != 0);
        REQUIRE(Value(ext2->GetAttributes(file)).size == 3 * GiB + 3);
        const NodeId grown = Value(ext2->Create(ext2->Root(), "grown.bin", 0644));
        Succeeds(ext2->Truncate(grown, 5 * GiB));
    }
    CheckClean(*device, "large-file");
    {
        MountedFilesystem ext2(Ext2Type(), *device, true);
        REQUIRE(ReadAt(*ext2, Value(Walk(*ext2, "large.bin")), 3 * GiB - 2, 10) ==
                std::string(2, '\0') + "far");
        REQUIRE(Value(ext2->GetAttributes(Value(Walk(*ext2, "grown.bin")))).size == 5 * GiB);
    }

    // Revision 0 has no feature flags to set, so its files stay below 2 GiB.
    const std::unique_ptr<MemoryDevice> old = DeviceFrom(LoadExtImage("rev0"));
    {
        MountedFilesystem ext2(Ext2Type(), *old);
        REQUIRE(ext2->GetCapabilities().maximum_file_size == 0x7FFFFFFF);
        const NodeId file = Value(ext2->Create(ext2->Root(), "large.bin", 0644));
        REQUIRE(ErrorOf(ext2->Write(file, 3 * GiB, BytesOfText("far"))) == Error::FileTooLarge);
        REQUIRE(WriteAt(*ext2, file, 0x7FFFFFFE, "far") == 1);
    }
    CheckClean(*old, "large-file-rev0");
#endif
}

#ifdef FEATHERDISK_EXT_IMAGES
namespace
{

// Ignores every failure: after the cut, everything that writes fails.
void CrashScenario(Filesystem& ext2)
{
    const NodeId root = ext2.Root();
    const Result<NodeId> file = ext2.Create(root, "crash.bin", 0644);
    if (file)
    {
        const std::string text = Pattern(40 * 1024);
        static_cast<void>(ext2.Write(*file, 0, BytesOfText(text.substr(0, 20000))));
        static_cast<void>(ext2.Write(*file, 20000, BytesOfText(text.substr(20000))));
        static_cast<void>(ext2.Truncate(*file, 15 * 1024 + 3));
        static_cast<void>(ext2.Write(*file, 300 * 1024, BytesOfText("tail")));
    }
    for (int number = 0; number < 12; ++number)
    {
        if (const Result<NodeId> made = ext2.Create(root, NumberedName("crash", number, 120), 0644))
        {
            static_cast<void>(
                ext2.Write(*made, 0, BytesOfText(NumberedName("data", number, 1500))));
        }
    }
    static_cast<void>(ext2.Remove(root, "hello.txt"));
    static_cast<void>(ext2.Remove(root, "big.txt"));
    static_cast<void>(ext2.Remove(root, "crash.bin"));
    for (int number = 0; number < 12; number += 3)
    {
        static_cast<void>(ext2.Remove(root, NumberedName("crash", number, 120)));
    }
    if (const Result<NodeId> hard = Walk(ext2, "hard.txt"))
    {
        AttributeChanges mode{};
        mode.mode = 0600;
        static_cast<void>(ext2.SetAttributes(*hard, mode));
    }
}

} // namespace
#endif

TEST_CASE("an ext2 volume stopped after any write needs only e2fsck's routine repairs")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    if (!HaveE2fsck())
    {
        SKIP("e2fsck is not installed");
    }
    // ext4-csum adds checksums and groups whose first use writes their descriptor, and
    // ext4-nojournal extents.
    for (const std::string_view variant : {"1k", "ext4-csum", "ext4-nojournal"})
    {
        INFO(variant);
        const std::vector<std::uint8_t> original = LoadExtImage(variant);
        std::uint64_t total = 0;
        {
            const std::unique_ptr<MemoryDevice> device = DeviceFrom(original);
            {
                MountedFilesystem ext2(Ext2Type(), *device);
                CrashScenario(*ext2);
            }
            total = device->write_count;
            CheckClean(*device, "crash-complete");
        }
        REQUIRE(total > 100);
        for (std::uint64_t cut = 1; cut < total; ++cut)
        {
            INFO("stopped after write " << cut << " of " << total);
            const std::unique_ptr<MemoryDevice> device = DeviceFrom(original);
            device->FailWritesAfter(cut);
            const Result<Filesystem*> mounted =
                Ext2Type().Mount(Mountable(*device), {.read_only = false});
            REQUIRE(mounted.has_value());
            CrashScenario(**mounted);
            const std::unique_ptr<MemoryDevice> stopped = CopyOf(*device);
            // The mount's memory goes; what it would still write lands after the snapshot.
            device->FailWritesAfter(UINT64_MAX);
            Succeeds(Ext2Type().Unmount(**mounted));
            const ToolRun report = RunE2fsck(*stopped, "crash");
            INFO(report.output);
            REQUIRE(OnlyRoutineRepairs(report.output));
        }
    }
#endif
}

#ifdef FEATHERDISK_EXT_IMAGES
namespace
{

enum class Kind
{
    File,
    Directory,
    Link,
};

struct ModelEntry
{
    Kind kind;
    // A file's contents, shared by its hard links.
    int file;
    std::string target;
    // Directories of the image, whose other entries the model does not know: never renamed or
    // removed.
    bool fixed;
};

// Paths from the root without a leading '/'; the root itself is "".
struct Model
{
    std::map<std::string, ModelEntry> entries;
    std::map<int, std::string> contents;
    int next_file = 0;
};

std::string RandomName(std::mt19937& random)
{
    constexpr std::string_view Characters = "abcdefghijklmnopqrstuvwxyz0123456789-_.";
    const std::size_t length = random() % 8 == 0 ? 150 + random() % 100 : 1 + random() % 24;
    std::string name;
    for (std::size_t index = 0; index < length; ++index)
    {
        name += Characters[random() % Characters.size()];
    }
    return name == "." || name == ".." ? "dots" : name;
}

std::string RandomBytes(std::mt19937& random, std::size_t length)
{
    std::string bytes(length, '\0');
    const auto seed = static_cast<char>(random());
    for (std::size_t index = 0; index < length; ++index)
    {
        bytes[index] = static_cast<char>(seed + static_cast<char>(index * 7 + index / 251));
    }
    return bytes;
}

std::pair<std::string, std::string> SplitPath(const std::string& path)
{
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos)
    {
        return {"", path};
    }
    return {path.substr(0, slash), path.substr(slash + 1)};
}

std::string Join(const std::string& directory, const std::string& name)
{
    return directory.empty() ? name : directory + "/" + name;
}

bool IsWithin(const std::string& path, const std::string& directory)
{
    return path == directory ||
           (path.size() > directory.size() && path.compare(0, directory.size(), directory) == 0 &&
            path[directory.size()] == '/');
}

std::vector<std::string> Paths(const Model& model, Kind kind, bool movable)
{
    std::vector<std::string> paths;
    for (const auto& [path, entry] : model.entries)
    {
        if (entry.kind == kind && !(movable && entry.fixed))
        {
            paths.push_back(path);
        }
    }
    return paths;
}

std::vector<std::string> Directories(const Model& model)
{
    std::vector<std::string> directories = Paths(model, Kind::Directory, false);
    directories.emplace_back();
    return directories;
}

// The names in directory, as List gives them.
std::vector<std::string> Children(const Model& model, const std::string& directory)
{
    std::vector<std::string> names;
    for (const auto& [path, entry] : model.entries)
    {
        if (!path.empty() && SplitPath(path).first == directory)
        {
            names.push_back(SplitPath(path).second + (entry.kind == Kind::Directory ? "/" : ""));
        }
    }
    return Sorted(names);
}

std::uint32_t NameCount(const Model& model, int file)
{
    std::uint32_t count = 0;
    for (const auto& [path, entry] : model.entries)
    {
        count += entry.kind == Kind::File && entry.file == file ? 1 : 0;
    }
    return count;
}

void Forget(Model& model, const std::string& path)
{
    const ModelEntry entry = model.entries.at(path);
    model.entries.erase(path);
    if (entry.kind == Kind::File && NameCount(model, entry.file) == 0)
    {
        model.contents.erase(entry.file);
    }
}

void Verify(Filesystem& ext2, const Model& model)
{
    for (const auto& [path, entry] : model.entries)
    {
        INFO(path);
        const NodeId node = Value(Walk(ext2, path));
        const Attributes attributes = Value(ext2.GetAttributes(node));
        switch (entry.kind)
        {
        case Kind::File:
            REQUIRE(attributes.type == NodeType::File);
            REQUIRE(ReadAll(ext2, node) == model.contents.at(entry.file));
            REQUIRE(attributes.link_count == NameCount(model, entry.file));
            break;
        case Kind::Link:
        {
            REQUIRE(attributes.type == NodeType::SymbolicLink);
            std::string target(1024, '\0');
            target.resize(Value(ext2.ReadLink(node, std::span(target.data(), target.size()))));
            REQUIRE(target == entry.target);
            break;
        }
        case Kind::Directory:
            REQUIRE(attributes.type == NodeType::Directory);
            if (!entry.fixed)
            {
                const std::vector<std::string> children = Children(model, path);
                REQUIRE(Sorted(List(ext2, node)) == children);
                REQUIRE(Value(ext2.Lookup(node, "..")) == Value(Walk(ext2, SplitPath(path).first)));
                REQUIRE(attributes.link_count == 2 + std::count_if(children.begin(), children.end(),
                                                                   [](const std::string& name)
                                                                   { return name.back() == '/'; }));
            }
            break;
        }
    }
}

template <typename T> const T& Pick(std::mt19937& random, const std::vector<T>& from)
{
    return from[random() % from.size()];
}

// What Rename must answer for the model, in the order the driver checks.
std::optional<Error> RenameRefusal(const Model& model, const std::string& from,
                                   const std::string& to)
{
    const ModelEntry& source = model.entries.at(from);
    const auto existing = model.entries.find(to);
    if (existing != model.entries.end())
    {
        if (from == to || (source.kind == Kind::File && existing->second.kind == Kind::File &&
                           source.file == existing->second.file))
        {
            return std::nullopt;
        }
        const bool source_directory = source.kind == Kind::Directory;
        const bool target_directory = existing->second.kind == Kind::Directory;
        if (source_directory != target_directory)
        {
            return source_directory ? Error::NotDirectory : Error::IsDirectory;
        }
        if (target_directory && (existing->second.fixed || !Children(model, to).empty()))
        {
            return Error::NotEmpty;
        }
    }
    if (source.kind == Kind::Directory && SplitPath(from).first != SplitPath(to).first &&
        IsWithin(SplitPath(to).first, from))
    {
        return Error::InvalidArgument;
    }
    return std::nullopt;
}

void ApplyRename(Model& model, const std::string& from, const std::string& to)
{
    const auto existing = model.entries.find(to);
    if (existing != model.entries.end())
    {
        const ModelEntry& source = model.entries.at(from);
        if (from == to || (source.kind == Kind::File && existing->second.kind == Kind::File &&
                           source.file == existing->second.file))
        {
            return;
        }
        Forget(model, to);
    }
    std::map<std::string, ModelEntry> moved;
    for (auto entry = model.entries.begin(); entry != model.entries.end();)
    {
        if (IsWithin(entry->first, from))
        {
            moved[to + entry->first.substr(from.size())] = entry->second;
            entry = model.entries.erase(entry);
        }
        else
        {
            ++entry;
        }
    }
    model.entries.insert(moved.begin(), moved.end());
}

void RandomChanges(std::string_view variant, std::uint32_t seed, int operations, std::uint64_t far)
{
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(variant));
    std::mt19937 random(seed);
    Model model;
    model.entries["a/b"] = {.kind = Kind::Directory, .file = 0, .target = {}, .fixed = true};
    model.entries["many"] = {.kind = Kind::Directory, .file = 0, .target = {}, .fixed = true};
    auto ext2 = std::make_unique<MountedFilesystem>(Ext2Type(), *device);
    const std::string big = ReadAll(**ext2, Value(Walk(**ext2, "big.txt")));
    int checks = 0;
    // A name new to the volume in a random directory, or nothing when it is taken by an entry
    // of the image the model does not know.
    const auto new_path = [&](Filesystem& filesystem) -> std::optional<std::string>
    {
        const std::string directory = Pick(random, Directories(model));
        const std::string path = Join(directory, RandomName(random));
        if (model.entries.contains(path) || Walk(filesystem, path))
        {
            return std::nullopt;
        }
        return path;
    };
    const auto allowed = [](const Result<NodeId>& result)
    {
        if (!result)
        {
            REQUIRE(result.error() == Error::NoSpace);
        }
        return result.has_value();
    };
    for (int operation = 0; operation < operations; ++operation)
    {
        Filesystem& filesystem = **ext2;
        const std::vector<std::string> files = Paths(model, Kind::File, false);
        const std::uint32_t choice = random() % 100;
        INFO("operation " << operation << ", choice " << choice);
        if (files.empty() || choice < 14)
        {
            const std::optional<std::string> path = new_path(filesystem);
            if (!path)
            {
                continue;
            }
            const auto [directory, name] = SplitPath(*path);
            if (allowed(filesystem.Create(Value(Walk(filesystem, directory)), name, 0644)))
            {
                model.entries[*path] = {
                    .kind = Kind::File, .file = model.next_file, .target = {}, .fixed = false};
                model.contents[model.next_file++] = "";
            }
        }
        else if (choice < 40)
        {
            const std::string path = Pick(random, files);
            std::string& contents = model.contents[model.entries.at(path).file];
            std::uint64_t offset = 0;
            switch (random() % 4)
            {
            case 0:
                offset = contents.empty() ? 0 : random() % contents.size();
                break;
            case 1:
                offset = contents.size();
                break;
            case 2:
                offset = contents.size() + random() % 5000;
                break;
            default:
                offset = random() % far;
                break;
            }
            const std::size_t length = 1 + random() % (random() % 4 == 0 ? 40000 : 3000);
            const std::string data = RandomBytes(random, length);
            const Result<std::size_t> written =
                filesystem.Write(Value(Walk(filesystem, path)), offset, BytesOfText(data));
            std::size_t count = 0;
            if (written)
            {
                count = *written;
            }
            else
            {
                REQUIRE(written.error() == Error::NoSpace);
            }
            if (count > 0)
            {
                if (contents.size() < offset + count)
                {
                    contents.resize(static_cast<std::size_t>(offset + count), '\0');
                }
                contents.replace(static_cast<std::size_t>(offset), count, data, 0, count);
            }
        }
        else if (choice < 50)
        {
            const std::string path = Pick(random, files);
            std::string& contents = model.contents[model.entries.at(path).file];
            std::uint64_t size = 0;
            switch (random() % 3)
            {
            case 0:
                size = contents.empty() ? 0 : random() % contents.size();
                break;
            case 1:
                size = contents.size() + random() % 20000;
                break;
            default:
                size = random() % far;
                break;
            }
            Succeeds(filesystem.Truncate(Value(Walk(filesystem, path)), size));
            contents.resize(static_cast<std::size_t>(size), '\0');
        }
        else if (choice < 58)
        {
            std::vector<std::string> removable = files;
            const std::vector<std::string> links = Paths(model, Kind::Link, false);
            removable.insert(removable.end(), links.begin(), links.end());
            const std::string path = Pick(random, removable);
            const auto [directory, name] = SplitPath(path);
            Succeeds(filesystem.Remove(Value(Walk(filesystem, directory)), name));
            Forget(model, path);
        }
        else if (choice < 64)
        {
            const std::optional<std::string> path = new_path(filesystem);
            if (!path)
            {
                continue;
            }
            const auto [directory, name] = SplitPath(*path);
            if (allowed(filesystem.MakeDirectory(Value(Walk(filesystem, directory)), name, 0755)))
            {
                model.entries[*path] = {
                    .kind = Kind::Directory, .file = 0, .target = {}, .fixed = false};
            }
        }
        else if (choice < 68)
        {
            const std::vector<std::string> directories = Paths(model, Kind::Directory, true);
            if (directories.empty())
            {
                continue;
            }
            const std::string path = Pick(random, directories);
            const auto [directory, name] = SplitPath(path);
            const Status removed =
                filesystem.RemoveDirectory(Value(Walk(filesystem, directory)), name);
            if (Children(model, path).empty())
            {
                Succeeds(removed);
                model.entries.erase(path);
            }
            else
            {
                REQUIRE(removed.error() == Error::NotEmpty);
            }
        }
        else if (choice < 80)
        {
            std::vector<std::string> movable = Paths(model, Kind::Directory, true);
            const std::vector<std::string> links = Paths(model, Kind::Link, false);
            movable.insert(movable.end(), files.begin(), files.end());
            movable.insert(movable.end(), links.begin(), links.end());
            const std::string from = Pick(random, movable);
            std::string to;
            const std::string directory = Pick(random, Directories(model));
            const std::vector<std::string> taken = Children(model, directory);
            if (random() % 2 == 0 && !taken.empty())
            {
                std::string name = Pick(random, taken);
                if (name.back() == '/')
                {
                    name.pop_back();
                }
                to = Join(directory, name);
            }
            else
            {
                to = Join(directory, RandomName(random));
                if (!model.entries.contains(to) && Walk(filesystem, to))
                {
                    continue;
                }
            }
            INFO("rename " << from << " to " << to);
            const std::optional<Error> refusal = RenameRefusal(model, from, to);
            const Status renamed = filesystem.Rename(
                Value(Walk(filesystem, SplitPath(from).first)), SplitPath(from).second,
                Value(Walk(filesystem, SplitPath(to).first)), SplitPath(to).second);
            if (!renamed && renamed.error() == Error::NoSpace)
            {
                continue;
            }
            if (refusal)
            {
                REQUIRE(renamed.error() == *refusal);
            }
            else
            {
                Succeeds(renamed);
                ApplyRename(model, from, to);
            }
        }
        else if (choice < 84)
        {
            const std::optional<std::string> path = new_path(filesystem);
            if (!path)
            {
                continue;
            }
            const std::string source = Pick(random, files);
            const auto [directory, name] = SplitPath(*path);
            const Status linked = filesystem.Link(Value(Walk(filesystem, source)),
                                                  Value(Walk(filesystem, directory)), name);
            if (!linked)
            {
                REQUIRE(linked.error() == Error::NoSpace);
                continue;
            }
            model.entries[*path] = model.entries.at(source);
        }
        else if (choice < 88)
        {
            const std::optional<std::string> path = new_path(filesystem);
            if (!path)
            {
                continue;
            }
            const std::string target = RandomName(random) + "/" + RandomName(random);
            const auto [directory, name] = SplitPath(*path);
            if (allowed(filesystem.SymbolicLink(Value(Walk(filesystem, directory)), name, target)))
            {
                model.entries[*path] = {
                    .kind = Kind::Link, .file = 0, .target = target, .fixed = false};
            }
        }
        else if (choice < 92)
        {
            ext2.reset();
            if (checks++ % 4 == 0)
            {
                CheckClean(*device, std::string(variant) + "-random-midway");
            }
            ext2 = std::make_unique<MountedFilesystem>(Ext2Type(), *device);
        }
        else
        {
            const std::string path = Pick(random, files);
            REQUIRE(ReadAll(filesystem, Value(Walk(filesystem, path))) ==
                    model.contents.at(model.entries.at(path).file));
        }
    }
    Verify(**ext2, model);
    ext2.reset();
    CheckClean(*device, std::string(variant) + "-random");
    MountedFilesystem check(Ext2Type(), *device, true);
    Verify(*check, model);
    REQUIRE(ReadAll(*check, Value(Walk(*check, "big.txt"))) == big);
    for (const std::string& directory : {std::string(), std::string("a/b"), std::string("many")})
    {
        const std::vector<std::string> listed = List(*check, Value(Walk(*check, directory)));
        for (const std::string& name : Children(model, directory))
        {
            REQUIRE(std::count(listed.begin(), listed.end(), name) == 1);
        }
    }
}

} // namespace
#endif

TEST_CASE("random ext2 file and directory changes match a model")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    // Far offsets reach double indirect blocks: past 268 KiB with 1 KiB blocks, past 1 MiB with
    // 2 KiB ones, past 4 MiB with 4 KiB ones.
    RandomChanges("1k", 2026, 1200, 600 * 1024);
    RandomChanges("4k", 1114, 600, 6 * 1024 * 1024);
    RandomChanges("ext4-csum", 2027, 800, 600 * 1024);
    RandomChanges("ext4-csum64", 2028, 500, 2 * 1024 * 1024);
    RandomChanges("ext4-gdtcsum", 2029, 400, 6 * 1024 * 1024);
    RandomChanges("ext4-nojournal", 2030, 800, 2 * 1024 * 1024);
    RandomChanges("ext4-mixed", 2031, 500, 6 * 1024 * 1024);
    // Through the journal: indirect blocks, extents, 64bit and 4 KiB blocks.
    RandomChanges("ext3", 2032, 800, 600 * 1024);
    RandomChanges("ext4", 2033, 800, 2 * 1024 * 1024);
    RandomChanges("ext4-64bit", 2034, 500, 2 * 1024 * 1024);
    RandomChanges("ext4-4k", 2035, 400, 6 * 1024 * 1024);
#endif
}
