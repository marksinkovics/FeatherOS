// SPDX-License-Identifier: MIT OR Apache-2.0
// ext2 over images that mke2fs, an independent implementation, made (tests/make_ext2_image.sh).
// Without e2fsprogs the images are not made and these tests are skipped.
#include <catch2/catch_test_macros.hpp>

#include "fs/ext2.h"
#include "fs/fat.h"
#include "fs/partition.h"
#include "tests/fat_image.h"
#include "tests/ext2_image.h"
#include "tests/filesystem_helpers.h"
#include "tests/memory_device.h"
#include "tests/partition_image.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef FEATHERDISK_EXT_IMAGES
namespace
{

using FeatherDisk::Attributes;
using FeatherDisk::Capabilities;
using FeatherDisk::DirectoryEntry;
using FeatherDisk::Error;
using FeatherDisk::Filesystem;
using FeatherDisk::NodeId;
using FeatherDisk::NodeType;
using FeatherDisk::Result;
using FeatherDisk::VolumeStatistics;
using FeatherDisk::Ext::Ext2Type;
using FeatherDisk::Fat::Fat16Type;
using FeatherDisk::Partition::Range;
using FeatherDisk::Partition::Volume;
using FeatherDisk::Tests::DeviceFrom;
using FeatherDisk::Tests::ErrorOf;
using FeatherDisk::Tests::Fat16Image;
using FeatherDisk::Tests::LinuxFilesystemType;
using FeatherDisk::Tests::List;
using FeatherDisk::Tests::LoadExtImage;
using FeatherDisk::Tests::MemoryDevice;
using FeatherDisk::Tests::MountedFilesystem;
using FeatherDisk::Tests::RawVolume;
using FeatherDisk::Tests::ReadAll;
using FeatherDisk::Tests::SetMbrEntry;
using FeatherDisk::Tests::Sorted;
using FeatherDisk::Tests::Succeeds;
using FeatherDisk::Tests::Value;
using FeatherDisk::Tests::Walk;
using FeatherDisk::Tests::WriteGpt;

constexpr std::uint32_t SectorSize = 512;
constexpr std::uint64_t VolumeBytes = 8 * 1024 * 1024;
constexpr std::uint64_t SparseTail = 73400320;
constexpr std::uint64_t SparseMiddle = 300000;
constexpr std::string_view SlowTarget =
    "a/b/c/a-symbolic-link-target-that-is-longer-than-sixty-bytes.txt";

std::string BigText()
{
    std::string text;
    for (int line = 0; line < 12800; ++line)
    {
        std::array<char, 64> buffer{};
        std::snprintf(buffer.data(), buffer.size(), "line %05d of a file with indirect\n", line);
        text += buffer.data();
    }
    return text;
}

std::string ReadAt(Filesystem& filesystem, NodeId file, std::uint64_t offset, std::size_t length)
{
    std::string text(length, '\0');
    const std::size_t count = Value(filesystem.Read(
        file, offset, std::span(reinterpret_cast<std::uint8_t*>(text.data()), text.size())));
    text.resize(count);
    return text;
}

std::string LinkTarget(Filesystem& filesystem, NodeId link)
{
    std::array<char, 4096> buffer{};
    const std::size_t count = Value(filesystem.ReadLink(link, buffer));
    return {buffer.data(), count};
}

std::map<std::string, NodeType> Types(Filesystem& filesystem, NodeId directory)
{
    std::map<std::string, NodeType> types;
    std::uint64_t cookie = 0;
    DirectoryEntry entry{};
    while (Value(filesystem.ReadDirectory(directory, cookie, entry)))
    {
        types[std::string(entry.name.data(), entry.name_length)] = entry.type;
    }
    return types;
}

std::vector<std::string> ManyNames()
{
    std::vector<std::string> names;
    for (int number = 0; number < 300; ++number)
    {
        std::array<char, 64> name{};
        std::snprintf(name.data(), name.size(), "entry-with-a-longer-name-%03d.txt", number);
        names.emplace_back(name.data());
    }
    return names;
}

struct Expected
{
    std::uint32_t block_size;
    // Large inodes extend the times to nanoseconds.
    bool nanoseconds;
};

void CheckReadOnly(Filesystem& ext2)
{
    const NodeId root = ext2.Root();
    const NodeId hello = Value(Walk(ext2, "hello.txt"));
    const std::array<std::uint8_t, 1> byte{};
    REQUIRE_FALSE(ext2.GetCapabilities().writable);
    REQUIRE(ErrorOf(ext2.Write(hello, 0, byte)) == Error::ReadOnly);
    REQUIRE(ext2.Truncate(hello, 0).error() == Error::ReadOnly);
    FeatherDisk::AttributeChanges changes{};
    changes.mode = 0600;
    REQUIRE(ext2.SetAttributes(hello, changes).error() == Error::ReadOnly);
    REQUIRE(ErrorOf(ext2.Create(root, "new.txt", 0644)) == Error::ReadOnly);
    REQUIRE(ErrorOf(ext2.MakeDirectory(root, "new", 0755)) == Error::ReadOnly);
    REQUIRE(ext2.Remove(root, "hello.txt").error() == Error::ReadOnly);
    REQUIRE(ext2.RemoveDirectory(root, "a").error() == Error::ReadOnly);
    REQUIRE(ext2.Rename(root, "hello.txt", root, "renamed.txt").error() == Error::ReadOnly);
    REQUIRE(ext2.Link(hello, root, "again.txt").error() == Error::ReadOnly);
    REQUIRE(ErrorOf(ext2.SymbolicLink(root, "link", "hello.txt")) == Error::ReadOnly);
    Succeeds(ext2.Sync());
    REQUIRE(ReadAll(ext2, hello) == "Hello from ext2.\n");
}

void CheckVolume(Filesystem& ext2, Expected expected)
{
    const NodeId root = ext2.Root();
    REQUIRE(root == 2);
    REQUIRE(Sorted(List(ext2, root)) ==
            std::vector<std::string>{"a/", "big.txt", "empty.txt", "fast-link", "hard.txt",
                                     "hello.txt", "lost+found/", "many/", "pipe", "slow-link",
                                     "sparse.bin", "tool"});
    const std::map<std::string, NodeType> types = Types(ext2, root);
    REQUIRE(types.at("pipe") == NodeType::Special);
    REQUIRE(types.at("fast-link") == NodeType::SymbolicLink);
    REQUIRE(types.at("tool") == NodeType::File);
    REQUIRE(types.at("a") == NodeType::Directory);

    // A file with fixed attributes, and a hard link to it.
    const NodeId hello = Value(Walk(ext2, "hello.txt"));
    REQUIRE(ReadAll(ext2, hello) == "Hello from ext2.\n");
    REQUIRE(Value(Walk(ext2, "hard.txt")) == hello);
    REQUIRE(ErrorOf(Walk(ext2, "HELLO.TXT")) == Error::NotFound);
    const Attributes attributes = Value(ext2.GetAttributes(hello));
    REQUIRE(attributes.node == hello);
    REQUIRE(attributes.type == NodeType::File);
    REQUIRE(attributes.size == 17);
    REQUIRE(attributes.mode == 0640);
    REQUIRE(attributes.owner == 100000);
    REQUIRE(attributes.group == 4242);
    REQUIRE(attributes.link_count == 2);
    REQUIRE(attributes.access_time.seconds == 1600000000);
    REQUIRE(attributes.modify_time.seconds == 1577934245);
    REQUIRE(attributes.modify_time.nanoseconds == (expected.nanoseconds ? 1000U : 0U));
    REQUIRE(attributes.change_time.seconds == 1614834367);
    REQUIRE(ReadAll(ext2, Value(Walk(ext2, "empty.txt"))).empty());
    const Attributes tool = Value(ext2.GetAttributes(Value(Walk(ext2, "tool"))));
    REQUIRE(tool.mode == 04755);
    REQUIRE(tool.size == 10);
    const Attributes pipe = Value(ext2.GetAttributes(Value(Walk(ext2, "pipe"))));
    REQUIRE(pipe.type == NodeType::Special);
    REQUIRE(pipe.mode == 0644);

    // Direct, single and, with 1 KiB blocks, double indirect blocks.
    const std::string big = BigText();
    const NodeId big_file = Value(Walk(ext2, "big.txt"));
    REQUIRE(ReadAll(ext2, big_file) == big);
    REQUIRE(ReadAt(ext2, big_file, 1000, 3000) == big.substr(1000, 3000));
    REQUIRE(ReadAt(ext2, big_file, 300 * 1024 + 7, 5000) == big.substr(300 * 1024 + 7, 5000));
    REQUIRE(ReadAt(ext2, big_file, big.size() - 10, 100) == big.substr(big.size() - 10));
    REQUIRE(ReadAt(ext2, big_file, big.size(), 100).empty());
    REQUIRE(ReadAt(ext2, big_file, big.size() + 5000, 100).empty());

    // Holes read as zeros; the tail needs a triple indirect block with 1 KiB blocks.
    const NodeId sparse = Value(Walk(ext2, "sparse.bin"));
    REQUIRE(Value(ext2.GetAttributes(sparse)).size == SparseTail + 4);
    REQUIRE(ReadAt(ext2, sparse, 0, 4) == "head");
    REQUIRE(ReadAt(ext2, sparse, 0, 8192) == "head" + std::string(8188, '\0'));
    REQUIRE(ReadAt(ext2, sparse, SparseMiddle - 10, 30) ==
            std::string(10, '\0') + "middle" + std::string(14, '\0'));
    REQUIRE(ReadAt(ext2, sparse, 1024 * 1024, 65536) == std::string(65536, '\0'));
    REQUIRE(ReadAt(ext2, sparse, SparseTail - 6, 100) == std::string(6, '\0') + "tail");

    // Nested directories, "." and "..".
    REQUIRE(ReadAll(ext2, Value(Walk(ext2, "a/b/c/deep.txt"))) == "deep\n");
    REQUIRE(Value(Walk(ext2, "a/b/c/..")) == Value(Walk(ext2, "a/b")));
    REQUIRE(Value(Walk(ext2, "a/./b/../..")) == root);
    REQUIRE(Value(ext2.Lookup(root, "..")) == root);
    const Attributes directory = Value(ext2.GetAttributes(Value(Walk(ext2, "a"))));
    REQUIRE(directory.type == NodeType::Directory);
    REQUIRE(directory.mode == 0755);
    // Its own entry, ".", and ".." in b and in the long-named directory.
    REQUIRE(directory.link_count == 4);

    // A 200-byte name, four directories of them deep, as make_ext2_image.sh makes it.
    std::string long_name;
    while (long_name.size() < 200)
    {
        long_name += "a-long-name-";
    }
    long_name.resize(200);
    std::string deep_path = "a";
    for (int level = 0; level < 5; ++level)
    {
        deep_path += "/" + long_name;
    }
    REQUIRE(deep_path.size() == 1006);
    REQUIRE(ReadAll(ext2, Value(Walk(ext2, deep_path))) == "far\n");
    REQUIRE(Sorted(List(ext2, Value(Walk(ext2, "a")))) ==
            std::vector<std::string>{long_name + "/", "b/"});

    // Fast and slow symbolic links.
    const NodeId fast = Value(Walk(ext2, "fast-link"));
    const Attributes fast_attributes = Value(ext2.GetAttributes(fast));
    REQUIRE(fast_attributes.type == NodeType::SymbolicLink);
    REQUIRE(fast_attributes.size == 14);
    REQUIRE(fast_attributes.mode == 0777);
    REQUIRE(LinkTarget(ext2, fast) == "a/b/c/deep.txt");
    const NodeId slow = Value(Walk(ext2, "slow-link"));
    REQUIRE(Value(ext2.GetAttributes(slow)).size == SlowTarget.size());
    REQUIRE(LinkTarget(ext2, slow) == SlowTarget);
    std::array<char, 5> short_buffer{};
    REQUIRE(Value(ext2.ReadLink(slow, short_buffer)) == 5);
    REQUIRE(std::string_view(short_buffer.data(), 5) == "a/b/c");

    // A directory over several blocks, read in steps with the cookie.
    const NodeId many = Value(Walk(ext2, "many"));
    REQUIRE(Value(ext2.GetAttributes(many)).size > expected.block_size);
    REQUIRE(Sorted(List(ext2, many)) == ManyNames());
    REQUIRE(ReadAll(ext2, Value(Walk(ext2, "many/entry-with-a-longer-name-299.txt"))) ==
            "entry 299\n");
    DirectoryEntry entry{};
    std::uint64_t cookie = 5;
    REQUIRE(Value(ext2.ReadDirectory(many, cookie, entry)));
    REQUIRE(entry.name_length > 0);
    cookie = UINT64_MAX;
    REQUIRE_FALSE(Value(ext2.ReadDirectory(many, cookie, entry)));

    const VolumeStatistics statistics = Value(ext2.Statistics());
    REQUIRE(statistics.block_size == expected.block_size);
    REQUIRE(statistics.total_blocks * statistics.block_size == VolumeBytes);
    REQUIRE(statistics.free_blocks > 0);
    REQUIRE(statistics.free_blocks < statistics.total_blocks);
    REQUIRE(statistics.total_nodes > 320);
    REQUIRE(statistics.free_nodes < statistics.total_nodes - 320);

    const Capabilities capabilities = ext2.GetCapabilities();
    REQUIRE(capabilities.symbolic_links);
    REQUIRE(capabilities.hard_links);
    REQUIRE(capabilities.permissions);
    REQUIRE(capabilities.case_sensitive);
    REQUIRE(capabilities.maximum_name_length == 255);
    REQUIRE(capabilities.maximum_file_size > SparseTail);

    // Errors.
    std::array<std::uint8_t, 16> buffer{};
    REQUIRE(ErrorOf(ext2.Lookup(root, "missing")) == Error::NotFound);
    REQUIRE(ErrorOf(ext2.Lookup(hello, "x")) == Error::NotDirectory);
    REQUIRE(ErrorOf(ext2.Lookup(root, std::string(256, 'x'))) == Error::NameTooLong);
    REQUIRE(ErrorOf(ext2.ReadDirectory(hello, cookie, entry)) == Error::NotDirectory);
    REQUIRE(ErrorOf(ext2.Read(many, 0, buffer)) == Error::IsDirectory);
    REQUIRE(ErrorOf(ext2.Read(fast, 0, buffer)) == Error::InvalidArgument);
    REQUIRE(ErrorOf(ext2.Read(Value(Walk(ext2, "pipe")), 0, buffer)) == Error::InvalidArgument);
    std::array<char, 16> target{};
    REQUIRE(ErrorOf(ext2.ReadLink(hello, target)) == Error::InvalidArgument);
    REQUIRE(ErrorOf(ext2.GetAttributes(0)) == Error::InvalidArgument);
    REQUIRE(ErrorOf(ext2.GetAttributes(statistics.total_nodes + 1)) == Error::InvalidArgument);
    // Inode 1 lists bad blocks and has no links.
    REQUIRE(ErrorOf(ext2.GetAttributes(1)) == Error::NotFound);

    CheckReadOnly(ext2);
}

void CheckImage(std::string_view variant, Expected expected)
{
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(variant));
    REQUIRE(Ext2Type().Name() == "ext2");
    REQUIRE(Ext2Type().Probe(*device));
    REQUIRE_FALSE(Fat16Type().Probe(*device));
    MountedFilesystem ext2(Ext2Type(), *device, true);
    CheckVolume(*ext2, expected);
}

// Mounts with every metadata block the walk reaches read, bounded so that a damaged directory
// that leads back to its parent still ends.
void Exercise(Filesystem& filesystem, NodeId directory, int depth, int& budget)
{
    std::uint64_t cookie = 0;
    DirectoryEntry entry{};
    std::vector<std::uint8_t> buffer(4096);
    std::array<char, 256> target{};
    while (depth < 6 && budget-- > 0)
    {
        const Result<bool> more = filesystem.ReadDirectory(directory, cookie, entry);
        if (!more || !*more)
        {
            return;
        }
        static_cast<void>(filesystem.Lookup(directory, {entry.name.data(), entry.name_length}));
        const Result<Attributes> attributes = filesystem.GetAttributes(entry.node);
        static_cast<void>(filesystem.Read(entry.node, 0, buffer));
        if (attributes && attributes->size > 0)
        {
            static_cast<void>(filesystem.Read(entry.node, attributes->size - 1, buffer));
            static_cast<void>(filesystem.Read(entry.node, attributes->size / 2, buffer));
        }
        static_cast<void>(filesystem.ReadLink(entry.node, target));
        if (entry.type == NodeType::Directory && entry.node != directory)
        {
            Exercise(filesystem, entry.node, depth + 1, budget);
        }
    }
}

} // namespace
#endif

TEST_CASE("ext2 reads a volume with 1 KiB blocks and 128-byte inodes")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    CheckImage("1k", {.block_size = 1024, .nanoseconds = false});
#endif
}

TEST_CASE("ext2 reads a volume with 4 KiB blocks and 256-byte inodes")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    CheckImage("4k", {.block_size = 4096, .nanoseconds = true});
#endif
}

TEST_CASE("ext2 reads a volume with 2 KiB blocks and without the filetype feature")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    CheckImage("nofiletype", {.block_size = 2048, .nanoseconds = true});
#endif
}

TEST_CASE("ext2 reads a revision 0 volume")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    CheckImage("rev0", {.block_size = 1024, .nanoseconds = false});
#endif
}

TEST_CASE("an ext3 volume with a clean journal reads as ext2")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    CheckImage("ext3", {.block_size = 1024, .nanoseconds = true});
#endif
}

TEST_CASE("a read-only ext2 mount changes nothing on the volume")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage("1k"));
    {
        MountedFilesystem ext2(Ext2Type(), *device, true);
        CheckReadOnly(*ext2);
    }
    const std::vector<std::uint8_t> original = LoadExtImage("1k");
    for (std::uint64_t sector = 0; sector < device->SectorCount(); ++sector)
    {
        REQUIRE(std::memcmp(device->Sector(sector).data(), original.data() + sector * SectorSize,
                            SectorSize) == 0);
    }
    REQUIRE(device->flush_count == 0);
#endif
}

TEST_CASE("ext2 refuses volumes with incompatible features it does not support")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    const std::unique_ptr<MemoryDevice> ext4 = DeviceFrom(LoadExtImage("ext4"));
    REQUIRE(Ext2Type().Probe(*ext4));
    REQUIRE(ErrorOf(Ext2Type().Mount(*ext4, {.read_only = true})) == Error::Unsupported);

    // A journal that needs recovery, extents, and a bit no one has defined yet.
    for (const std::uint32_t feature : {0x0004U, 0x0040U, 0x80000000U})
    {
        std::vector<std::uint8_t> bytes = LoadExtImage("1k");
        RawVolume raw{bytes};
        raw.Store32(1024 + 96, raw.Load32(1024 + 96) | feature);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
        INFO("feature " << feature);
        REQUIRE(ErrorOf(Ext2Type().Mount(*device, {.read_only = true})) == Error::Unsupported);
    }

    // Read-only compatible features that are unknown still leave the volume readable.
    std::vector<std::uint8_t> bytes = LoadExtImage("1k");
    RawVolume raw{bytes};
    raw.Store32(1024 + 100, raw.Load32(1024 + 100) | 0x80000000U);
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
    MountedFilesystem ext2(Ext2Type(), *device, false);
    REQUIRE_FALSE(ext2->GetCapabilities().writable);
    REQUIRE(ReadAll(*ext2, Value(Walk(*ext2, "hello.txt"))) == "Hello from ext2.\n");
#endif
}

TEST_CASE("ext2 probes only ext2 volumes and needs blocks of whole sectors")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    Fat16Image fat;
    REQUIRE_FALSE(Ext2Type().Probe(fat.Device()));
    REQUIRE(ErrorOf(Ext2Type().Mount(fat.Device(), {.read_only = true})) == Error::Unsupported);
    MemoryDevice blank(512, 64);
    REQUIRE_FALSE(Ext2Type().Probe(blank));
    MemoryDevice tiny(512, 3);
    REQUIRE_FALSE(Ext2Type().Probe(tiny));
    MemoryDevice unreadable(512, 64);
    unreadable.FailReadsFrom(0);
    REQUIRE_FALSE(Ext2Type().Probe(unreadable));
    REQUIRE(ErrorOf(Ext2Type().Mount(unreadable, {.read_only = true})) == Error::Io);

    // With 4096-byte sectors the superblock is the second quarter of sector 0.
    const std::unique_ptr<MemoryDevice> large_sectors = DeviceFrom(LoadExtImage("4k"), 4096);
    {
        MountedFilesystem ext2(Ext2Type(), *large_sectors, true);
        REQUIRE(ReadAll(*ext2, Value(Walk(*ext2, "a/b/c/deep.txt"))) == "deep\n");
        REQUIRE(ReadAll(*ext2, Value(Walk(*ext2, "big.txt"))) == BigText());
    }
    const std::unique_ptr<MemoryDevice> small_blocks = DeviceFrom(LoadExtImage("1k"), 4096);
    REQUIRE(Ext2Type().Probe(*small_blocks));
    REQUIRE(ErrorOf(Ext2Type().Mount(*small_blocks, {.read_only = true})) == Error::Unsupported);
#endif
}

TEST_CASE("ext2 mounts the Linux partition of an MBR disk that mke2fs made")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    const std::unique_ptr<MemoryDevice> disk = DeviceFrom(LoadExtImage("mbr"));
    REQUIRE_FALSE(Ext2Type().Probe(*disk));
    REQUIRE(FeatherDisk::Partition::Identify(*disk) == FeatherDisk::Partition::Scheme::Mbr);
    std::array<Range, 4> ranges{};
    REQUIRE(FeatherDisk::Partition::Find(*disk, ranges) == 1);
    REQUIRE(ranges[0].first_sector == 2048);
    REQUIRE(ranges[0].sector_count == 16384);
    Volume volume(*disk, ranges[0]);
    REQUIRE_FALSE(Fat16Type().Probe(volume));
    MountedFilesystem ext2(Ext2Type(), volume, true);
    CheckVolume(*ext2, {.block_size = 1024, .nanoseconds = false});
#endif
}

#ifdef FEATHERDISK_EXT_IMAGES
namespace
{

constexpr std::uint64_t FatStart = 64;
constexpr std::uint64_t ExtStart = 8192;

// A FAT16 volume at FatStart and the 1k ext2 volume at ExtStart, for a table to describe.
std::unique_ptr<MemoryDevice> TwoVolumeDisk()
{
    Fat16Image fat;
    fat.AddRootFile("FAT16 beside ext2.\n");
    const std::vector<std::uint8_t> ext = LoadExtImage("1k");
    auto disk = std::make_unique<MemoryDevice>(SectorSize, ExtStart + ext.size() / SectorSize + 64);
    for (std::uint64_t sector = 0; sector < fat.Device().SectorCount(); ++sector)
    {
        std::memcpy(disk->Sector(FatStart + sector).data(), fat.Device().Sector(sector).data(),
                    SectorSize);
    }
    for (std::uint64_t sector = 0; sector < ext.size() / SectorSize; ++sector)
    {
        std::memcpy(disk->Sector(ExtStart + sector).data(), ext.data() + sector * SectorSize,
                    SectorSize);
    }
    return disk;
}

// What the kernel's CoreDriver::Mount does: the first partition the type probes.
std::optional<Range> FirstProbed(FeatherDisk::FilesystemType& type, MemoryDevice& disk)
{
    std::array<Range, 16> ranges{};
    const std::size_t count = FeatherDisk::Partition::Find(disk, ranges);
    for (std::size_t index = 0; index < count; ++index)
    {
        Volume volume(disk, ranges[index]);
        if (type.Probe(volume))
        {
            return ranges[index];
        }
    }
    return std::nullopt;
}

void CheckTwoVolumes(MemoryDevice& disk)
{
    const std::optional<Range> fat_range = FirstProbed(Fat16Type(), disk);
    const std::optional<Range> ext_range = FirstProbed(Ext2Type(), disk);
    REQUIRE(fat_range.has_value());
    REQUIRE(ext_range.has_value());
    REQUIRE(fat_range->first_sector == FatStart);
    REQUIRE(ext_range->first_sector == ExtStart);
    Volume fat_volume(disk, *fat_range);
    MountedFilesystem fat(Fat16Type(), fat_volume, true);
    REQUIRE(ReadAll(*fat, Value(Walk(*fat, "BLOCK.TXT"))) == "FAT16 beside ext2.\n");
    Volume ext_volume(disk, *ext_range);
    MountedFilesystem ext2(Ext2Type(), ext_volume, true);
    REQUIRE(ReadAll(*ext2, Value(Walk(*ext2, "hello.txt"))) == "Hello from ext2.\n");
}

} // namespace
#endif

TEST_CASE("FAT16 and ext2 each find their own partition, in an MBR or a GPT disk")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    const auto ext_sectors = static_cast<std::uint32_t>(VolumeBytes / SectorSize);
    SECTION("MBR, the Linux partition first")
    {
        const std::unique_ptr<MemoryDevice> disk = TwoVolumeDisk();
        SetMbrEntry(*disk, 0, 0x83, static_cast<std::uint32_t>(ExtStart), ext_sectors);
        SetMbrEntry(*disk, 1, 0x06, static_cast<std::uint32_t>(FatStart), 5000);
        CheckTwoVolumes(*disk);
    }
    SECTION("GPT, with the Linux filesystem data GUID")
    {
        const std::unique_ptr<MemoryDevice> disk = TwoVolumeDisk();
        WriteGpt(*disk, {{.index = 0,
                          .first = ExtStart,
                          .last = ExtStart + ext_sectors - 1,
                          .type = LinuxFilesystemType},
                         {.index = 1, .first = FatStart, .last = FatStart + 4999}});
        CheckTwoVolumes(*disk);
    }
#endif
}

TEST_CASE("damaged ext2 directory records are corrupt, never followed")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    const auto damaged = [](auto damage)
    {
        std::vector<std::uint8_t> bytes = LoadExtImage("1k");
        RawVolume raw{bytes};
        damage(raw);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
        MountedFilesystem ext2(Ext2Type(), *device, true);
        std::uint64_t cookie = 0;
        DirectoryEntry entry{};
        REQUIRE(ErrorOf(ext2->ReadDirectory(ext2->Root(), cookie, entry)) == Error::Corrupt);
        REQUIRE(ErrorOf(ext2->Lookup(ext2->Root(), "hello.txt")) == Error::Corrupt);
    };
    // "." is the first record of the root's first block.
    for (const std::uint16_t length : {0, 6, 14, 1028, 0xFFFC})
    {
        INFO("record length " << length);
        damaged([length](RawVolume& raw) { raw.Store16(raw.EntryOffset(2, ".") + 4, length); });
    }
    damaged([](RawVolume& raw) { raw.bytes[raw.EntryOffset(2, ".") + 6] = 200; });
    damaged([](RawVolume& raw) { raw.Store32(raw.EntryOffset(2, "."), raw.InodeCount() + 1); });
    damaged([](RawVolume& raw) { raw.bytes[raw.EntryOffset(2, ".") + 6] = 0; });
#endif
}

TEST_CASE("damaged ext2 block pointers and sizes are corrupt, never followed")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    std::vector<std::uint8_t> original = LoadExtImage("1k");
    RawVolume original_raw{original};
    const auto inode_of = [&](std::string_view name)
    { return original_raw.Load32(original_raw.EntryOffset(2, name)); };
    const std::uint32_t big = inode_of("big.txt");
    const std::uint32_t sparse = inode_of("sparse.bin");
    const std::uint32_t slow = inode_of("slow-link");
    const std::uint32_t many = inode_of("many");

    struct Damage
    {
        const char* what;
        std::uint64_t offset;
        std::uint32_t value;
        std::uint32_t node;
        std::uint64_t read_at;
    };
    const RawVolume& raw = original_raw;
    const std::uint64_t big_inode = raw.InodeOffset(big);
    const std::uint64_t sparse_inode = raw.InodeOffset(sparse);
    const std::vector<Damage> damages = {
        {"a direct block past the volume", big_inode + 40, raw.BlockCount() + 10, big, 0},
        {"a direct block naming the superblock", big_inode + 40, 1, big, 0},
        {"a single indirect block past the volume", big_inode + 40 + 12 * 4, raw.BlockCount(), big,
         12 * 1024},
        {"a pointer in the single indirect block", raw.BlockOffset(raw.InodeBlock(big, 12)),
         0xFFFFFFFF, big, 12 * 1024},
        {"a pointer in the double indirect block", raw.BlockOffset(raw.InodeBlock(big, 13)),
         0xFFFFFFF0, big, 268 * 1024},
        {"a pointer in the triple indirect block", raw.BlockOffset(raw.InodeBlock(sparse, 14)), 1,
         sparse, SparseTail},
        {"a size past the block map's reach", big_inode + 108, 0x10, big, std::uint64_t{32} << 30},
        {"an inline-data file", sparse_inode + 32, 0x10000000, sparse, 0},
        {"an extent-mapped file", sparse_inode + 32, 0x00080000, sparse, 0},
    };
    for (const Damage& damage : damages)
    {
        INFO(damage.what);
        std::vector<std::uint8_t> bytes = original;
        RawVolume{bytes}.Store32(damage.offset, damage.value);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
        MountedFilesystem ext2(Ext2Type(), *device, true);
        std::array<std::uint8_t, 2048> buffer{};
        const Error error = ErrorOf(ext2->Read(damage.node, damage.read_at, buffer));
        REQUIRE((error == Error::Corrupt || error == Error::Unsupported));
    }

    // A slow link longer than its block, and a directory larger than the blocks it holds.
    std::vector<std::uint8_t> bytes = original;
    RawVolume damaged{bytes};
    damaged.Store32(damaged.InodeOffset(slow) + 4, 2000);
    damaged.Store32(damaged.InodeOffset(many) + 4, 1024 * 1024);
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
    MountedFilesystem ext2(Ext2Type(), *device, true);
    std::array<char, 64> target{};
    REQUIRE(ErrorOf(ext2->ReadLink(slow, target)) == Error::Corrupt);
    std::uint64_t cookie = 0;
    DirectoryEntry entry{};
    REQUIRE(ErrorOf(ext2->ReadDirectory(many, cookie, entry)) == Error::Corrupt);
    REQUIRE(ErrorOf(ext2->Lookup(many, "entry-with-a-longer-name-000.txt")) == Error::Corrupt);
#endif
}

TEST_CASE("damaged ext2 counts, groups and root refuse the mount")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    const auto mount_error = [](auto damage)
    {
        std::vector<std::uint8_t> bytes = LoadExtImage("1k");
        RawVolume raw{bytes};
        damage(raw);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
        return ErrorOf(Ext2Type().Mount(*device, {.read_only = true}));
    };
    REQUIRE(mount_error([](RawVolume& raw) { raw.Store32(1024, raw.InodeCount() + 1); }) ==
            Error::Corrupt);
    REQUIRE(mount_error([](RawVolume& raw) { raw.Store32(1024, raw.InodeCount() - 1); }) ==
            Error::Corrupt);
    REQUIRE(mount_error([](RawVolume& raw) { raw.Store32(1024, 0); }) == Error::Unsupported);
    REQUIRE(mount_error([](RawVolume& raw) { raw.Store32(1024 + 4, raw.BlockCount() * 2); }) ==
            Error::Corrupt);
    REQUIRE(mount_error([](RawVolume& raw)
                        { raw.Store32(raw.DescriptorOffset() + 8, raw.BlockCount() - 1); }) ==
            Error::Corrupt);
    REQUIRE(mount_error([](RawVolume& raw) { raw.Store32(raw.DescriptorOffset() + 8, 0); }) ==
            Error::Corrupt);
    REQUIRE(mount_error([](RawVolume& raw) { raw.Store16(raw.InodeOffset(2), 0100644); }) ==
            Error::Corrupt);
    REQUIRE(mount_error([](RawVolume& raw) { raw.Store16(raw.InodeOffset(2) + 26, 0); }) ==
            Error::Corrupt);
#endif
}

TEST_CASE("random damage to ext2 metadata never crashes the driver or makes it loop")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    std::vector<std::uint8_t> original = LoadExtImage("1k");
    const RawVolume raw{original};
    const std::uint32_t block_size = raw.BlockSize();
    const auto inode_of = [&](std::string_view name)
    { return raw.Load32(raw.EntryOffset(2, name)); };
    // The superblock's fields, the first group descriptor, the first 40 inodes, the root and
    // "many" directories' first blocks, and big.txt's indirect blocks.
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> regions = {
        {1024, 264},
        {raw.DescriptorOffset(), 32},
        {raw.InodeOffset(1), 40 * 128},
        {raw.BlockOffset(raw.InodeBlock(2, 0)), block_size},
        {raw.BlockOffset(raw.InodeBlock(inode_of("many"), 0)), block_size},
        {raw.BlockOffset(raw.InodeBlock(inode_of("big.txt"), 12)), block_size},
        {raw.BlockOffset(raw.InodeBlock(inode_of("big.txt"), 13)), block_size},
    };
    std::mt19937 generator(22);
    // One device, with each round's damage undone afterwards: building an 8 MiB device per round
    // costs more than the rounds themselves.
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(original);
    const auto byte_at = [&device](std::uint64_t offset) -> std::uint8_t&
    { return device->Sector(offset / SectorSize)[offset % SectorSize]; };
    int mounted = 0;
    for (int round = 0; round < 300; ++round)
    {
        std::vector<std::uint64_t> damaged;
        const std::uint32_t flips = 1 + generator() % 6;
        for (std::uint32_t flip = 0; flip < flips; ++flip)
        {
            const auto& [start, length] = regions[generator() % regions.size()];
            const std::uint64_t offset = start + generator() % length;
            byte_at(offset) ^= static_cast<std::uint8_t>(1 + generator() % 255);
            damaged.push_back(offset);
        }
        if (Ext2Type().Probe(*device))
        {
            const Result<Filesystem*> filesystem = Ext2Type().Mount(*device, {.read_only = true});
            if (filesystem)
            {
                ++mounted;
                int budget = 600;
                Exercise(**filesystem, (*filesystem)->Root(), 0, budget);
                static_cast<void>((*filesystem)->Statistics());
                Succeeds(Ext2Type().Unmount(**filesystem));
            }
        }
        for (const std::uint64_t offset : damaged)
        {
            byte_at(offset) = original[offset];
        }
    }
    REQUIRE(mounted > 100);
#endif
}
