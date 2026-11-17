// SPDX-License-Identifier: MIT OR Apache-2.0
// The ext3 and ext4 layout over images that mke2fs made (tests/make_ext2_image.sh): features the
// driver refuses, checksums verified on read, and groups that were never written. Without
// e2fsprogs the images are not made and these tests are skipped.
#include <catch2/catch_test_macros.hpp>

#include "ext/checksum.h"
#include "ext/superblock.h"
#include "fs/ext2.h"
#include "tests/ext2_image.h"
#include "tests/filesystem_helpers.h"
#include "tests/memory_device.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#ifdef FEATHERDISK_EXT_IMAGES
namespace
{

using FeatherDisk::DirectoryEntry;
using FeatherDisk::Error;
using FeatherDisk::NodeId;
using FeatherDisk::Ext::DecodeGroupDescriptor;
using FeatherDisk::Ext::DecodeSuperblock;
using FeatherDisk::Ext::Ext2Type;
using FeatherDisk::Ext::GroupDescriptor;
using FeatherDisk::Ext::Superblock;
using FeatherDisk::Tests::DeviceFrom;
using FeatherDisk::Tests::ErrorOf;
using FeatherDisk::Tests::LoadExtImage;
using FeatherDisk::Tests::MemoryDevice;
using FeatherDisk::Tests::MountedFilesystem;
using FeatherDisk::Tests::RawVolume;
using FeatherDisk::Tests::ReadAll;
using FeatherDisk::Tests::Value;
using FeatherDisk::Tests::Walk;

constexpr std::uint64_t SuperblockStart = 1024;
constexpr std::uint64_t IncompatibleOffset = SuperblockStart + 96;
constexpr std::uint64_t ReadOnlyCompatibleOffset = SuperblockStart + 100;

Superblock SuperblockOf(const std::vector<std::uint8_t>& bytes)
{
    return DecodeSuperblock(std::span(bytes).subspan(SuperblockStart).first<1024>());
}

GroupDescriptor DescriptorOf(RawVolume& raw, std::uint32_t group)
{
    return DecodeGroupDescriptor(std::span(raw.bytes).subspan(
        raw.DescriptorOffset() + group * raw.DescriptorSize(), raw.DescriptorSize()));
}

Error MountError(const std::vector<std::uint8_t>& bytes)
{
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
    return ErrorOf(Ext2Type().Mount(*device, {.read_only = true}));
}

std::uint32_t InodeOf(RawVolume& raw, std::uint32_t directory, std::string_view name)
{
    return raw.Load32(raw.EntryOffset(directory, name));
}

} // namespace
#endif

TEST_CASE("ext4 features the driver does not read yet refuse the mount")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    // compression, journal_dev, meta_bg, mmp, ea_inode, large_dir, inline_data, encrypt and
    // casefold, each with a superblock checksum that matches.
    for (const std::uint32_t feature :
         {0x0001U, 0x0008U, 0x0010U, 0x0100U, 0x0400U, 0x4000U, 0x8000U, 0x10000U, 0x20000U})
    {
        INFO("feature " << feature);
        std::vector<std::uint8_t> bytes = LoadExtImage("ext4-blockmap");
        RawVolume raw{bytes};
        raw.Store32(IncompatibleOffset, raw.Load32(IncompatibleOffset) | feature);
        raw.UpdateSuperblockChecksum();
        REQUIRE(MountError(bytes) == Error::Unsupported);
    }

    // bigalloc counts clusters in the bitmaps, and only CRC32C is a defined checksum type.
    std::vector<std::uint8_t> bytes = LoadExtImage("ext4-blockmap");
    RawVolume raw{bytes};
    raw.Store32(ReadOnlyCompatibleOffset, raw.Load32(ReadOnlyCompatibleOffset) | 0x0200U);
    raw.UpdateSuperblockChecksum();
    REQUIRE(MountError(bytes) == Error::Unsupported);
    bytes = LoadExtImage("ext4-blockmap");
    raw.bytes[SuperblockStart + 373] = 2;
    raw.UpdateSuperblockChecksum();
    REQUIRE(MountError(bytes) == Error::Unsupported);

    // An unknown read-only compatible feature (verity here) still reads.
    bytes = LoadExtImage("ext4-blockmap");
    raw.Store32(ReadOnlyCompatibleOffset, raw.Load32(ReadOnlyCompatibleOffset) | 0x8000U);
    raw.UpdateSuperblockChecksum();
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
    MountedFilesystem ext4(Ext2Type(), *device, false);
    REQUIRE_FALSE(ext4->GetCapabilities().writable);
    REQUIRE(ReadAll(*ext4, Value(Walk(*ext4, "hello.txt"))) == "Hello from ext2.\n");
#endif
}

TEST_CASE("ext4 describes its variant and features by e2fsprogs' names")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    using FeatherDisk::Ext::DescribeFeatures;
    using FeatherDisk::Ext::FeatureName;
    using FeatherDisk::Ext::FeatureSet;
    const auto variant = [](std::string_view image)
    {
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(image));
        return std::string(Value(DescribeFeatures(*device)).variant);
    };
    REQUIRE(variant("1k") == "ext2");
    REQUIRE(variant("ext3") == "ext3");
    REQUIRE(variant("ext4") == "ext4");
    REQUIRE(variant("ext4-gdtcsum") == "ext4");
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage("ext4-64bit"));
    const FeatherDisk::Ext::ExtFeatures features = Value(DescribeFeatures(*device));
    REQUIRE((features.incompatible & 0x80U) != 0);
    REQUIRE(FeatureName(FeatureSet::Incompatible, 0x80) == "64bit");
    REQUIRE(FeatureName(FeatureSet::Incompatible, 0x2000) == "metadata_csum_seed");
    REQUIRE(FeatureName(FeatureSet::ReadOnlyCompatible, 0x400) == "metadata_csum");
    REQUIRE(FeatureName(FeatureSet::ReadOnlyCompatible, 0x10) == "uninit_bg");
    REQUIRE(FeatureName(FeatureSet::Compatible, 0x4) == "has_journal");
    REQUIRE(FeatureName(FeatureSet::Compatible, 0x80000000).empty());
    REQUIRE(FeatureName(FeatureSet::Compatible, 0x5).empty());

    MemoryDevice empty(512, 64);
    REQUIRE(ErrorOf(DescribeFeatures(empty)) == Error::Unsupported);
#endif
}

TEST_CASE("the core's checksums match what mke2fs stored in every group")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    using namespace FeatherDisk::Ext;
    for (const std::string_view variant : {"ext4-blockmap", "ext4-64bit", "ext4-gdtcsum"})
    {
        INFO(variant);
        std::vector<std::uint8_t> bytes = LoadExtImage(variant);
        RawVolume raw{bytes};
        const Superblock superblock = SuperblockOf(bytes);
        REQUIRE(superblock.checksum_matches);
        const std::uint32_t seed = VolumeChecksumSeed(superblock);
        const bool metadata = HasMetadataChecksums(superblock);
        const std::uint32_t mask = superblock.descriptor_size >= 64 ? 0xFFFFFFFFU : 0xFFFFU;
        int block_bitmaps = 0;
        int inode_bitmaps = 0;
        for (std::uint32_t group = 0; group < superblock.group_count; ++group)
        {
            INFO("group " << group);
            const std::span<const std::uint8_t> descriptor_bytes = std::span(bytes).subspan(
                raw.DescriptorOffset() + group * raw.DescriptorSize(), raw.DescriptorSize());
            const GroupDescriptor descriptor = DescriptorOf(raw, group);
            REQUIRE(GroupDescriptorChecksum(superblock, seed, group, descriptor_bytes) ==
                    descriptor.checksum);
            if (!metadata)
            {
                continue;
            }
            // Bitmaps of groups never written carry no checksum.
            if ((descriptor.flags & GroupBlocksUnused) == 0)
            {
                const auto bitmap = std::span(bytes).subspan(
                    raw.BlockOffset(static_cast<std::uint32_t>(descriptor.block_bitmap)),
                    superblock.blocks_per_group / 8);
                REQUIRE((BitmapChecksum(seed, bitmap) & mask) == descriptor.block_bitmap_checksum);
                ++block_bitmaps;
            }
            if ((descriptor.flags & GroupInodesUnused) == 0)
            {
                const auto bitmap = std::span(bytes).subspan(
                    raw.BlockOffset(static_cast<std::uint32_t>(descriptor.inode_bitmap)),
                    superblock.inodes_per_group / 8);
                REQUIRE((BitmapChecksum(seed, bitmap) & mask) == descriptor.inode_bitmap_checksum);
                ++inode_bitmaps;
            }
        }
        if (metadata)
        {
            REQUIRE(block_bitmaps >= 2);
            REQUIRE(inode_bitmaps >= 1);
            const auto root = std::span(bytes).subspan(raw.InodeOffset(2), raw.InodeSize());
            REQUIRE(InodeChecksumMatches(
                root, InodeChecksumSeed(seed, 2, raw.Load32(raw.InodeOffset(2) + 100))));
        }
    }
#endif
}

TEST_CASE("a damaged ext4 superblock or group descriptor fails its checksum")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    // The volume label, which nothing else checks.
    std::vector<std::uint8_t> bytes = LoadExtImage("ext4-blockmap");
    bytes[SuperblockStart + 120] ^= 0x20;
    REQUIRE(MountError(bytes) == Error::Corrupt);

    // Group 1's free inode count (CRC32C, then CRC16), and with 64-byte descriptors also the
    // upper half of its free block count.
    for (const std::string_view variant : {"ext4-blockmap", "ext4-64bit", "ext4-gdtcsum"})
    {
        INFO(variant);
        bytes = LoadExtImage(variant);
        RawVolume raw{bytes};
        bytes[raw.DescriptorOffset() + raw.DescriptorSize() + 14] ^= 0x01;
        REQUIRE(MountError(bytes) == Error::Corrupt);
    }
    bytes = LoadExtImage("ext4-64bit");
    RawVolume raw{bytes};
    bytes[raw.DescriptorOffset() + raw.DescriptorSize() + 44] ^= 0x01;
    REQUIRE(MountError(bytes) == Error::Corrupt);
#endif
}

TEST_CASE("a damaged ext4 inode fails its checksum, the rest of the volume still reads")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    // i_atime, i_checksum_hi, the extended access time and a byte past i_extra_isize's fields:
    // the checksum covers the whole inode.
    for (const std::string_view variant : {"ext4-blockmap", "ext4-64bit"})
    {
        for (const std::uint64_t field : {8U, 124U, 130U, 140U, 200U})
        {
            INFO(variant << " byte " << field);
            std::vector<std::uint8_t> bytes = LoadExtImage(variant);
            RawVolume raw{bytes};
            const std::uint32_t hello = InodeOf(raw, 2, "hello.txt");
            bytes[raw.InodeOffset(hello) + field] ^= 0x10;
            const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
            MountedFilesystem ext4(Ext2Type(), *device, true);
            REQUIRE(ErrorOf(ext4->GetAttributes(hello)) == Error::Corrupt);
            std::array<std::uint8_t, 16> buffer{};
            REQUIRE(ErrorOf(ext4->Read(hello, 0, buffer)) == Error::Corrupt);
            REQUIRE(ReadAll(*ext4, Value(Walk(*ext4, "a/b/c/deep.txt"))) == "deep\n");
        }
    }

    // Without metadata_csum, inodes have no checksum to fail.
    std::vector<std::uint8_t> bytes = LoadExtImage("ext4-gdtcsum");
    RawVolume raw{bytes};
    const std::uint32_t hello = InodeOf(raw, 2, "hello.txt");
    bytes[raw.InodeOffset(hello) + 8] ^= 0x10;
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
    MountedFilesystem ext4(Ext2Type(), *device, true);
    REQUIRE(Value(ext4->GetAttributes(hello)).access_time.seconds == (1600000000 ^ 0x10));

    // i_extra_isize must keep the extra fields inside the inode, 4-byte aligned.
    bytes = LoadExtImage("ext4-gdtcsum");
    raw.Store16(raw.InodeOffset(hello) + 128, 30);
    const std::unique_ptr<MemoryDevice> misaligned = DeviceFrom(bytes);
    MountedFilesystem damaged(Ext2Type(), *misaligned, true);
    REQUIRE(ErrorOf(damaged->GetAttributes(hello)) == Error::Corrupt);
#endif
}

TEST_CASE("damaged ext4 directory blocks and hash index blocks fail their checksums")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    const std::vector<std::uint8_t> original = LoadExtImage("ext4-blockmap");
    std::vector<std::uint8_t> reference = original;
    RawVolume names{reference};
    const std::uint32_t block_size = names.BlockSize();
    const std::uint32_t a = InodeOf(names, 2, "a");
    const std::uint32_t many = InodeOf(names, 2, "many");
    // e2fsck -D gave many/ a hash index: its first block is the index root.
    REQUIRE((names.Load32(names.InodeOffset(many) + 32) & 0x1000U) != 0);

    struct Damage
    {
        const char* what;
        std::uint64_t offset;
        std::uint32_t directory;
    };
    const std::uint64_t a_block = names.BlockOffset(names.InodeBlock(a, 0));
    const std::uint64_t many_root = names.BlockOffset(names.InodeBlock(many, 0));
    const std::uint64_t many_leaf = names.BlockOffset(names.InodeBlock(many, 1));
    const std::vector<Damage> damages = {
        {"a name in a leaf block", names.EntryOffset(a, "b") + 8, a},
        {"the leaf block's checksum", a_block + block_size - 1, a},
        {"the checksum record's file type", a_block + block_size - 5, a},
        {"an entry of the index root", many_root + 40, many},
        {"the index root's limit", many_root + 32, many},
        {"a leaf block of the hashed directory", many_leaf + 20, many},
    };
    for (const Damage& damage : damages)
    {
        INFO(damage.what);
        std::vector<std::uint8_t> bytes = original;
        bytes[damage.offset] ^= 0x04;
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
        MountedFilesystem ext4(Ext2Type(), *device, true);
        std::uint64_t cookie = 0;
        DirectoryEntry entry{};
        Error error = Error::Io;
        // The walk may list records before the damaged block.
        for (int step = 0; step < 400; ++step)
        {
            const FeatherDisk::Result<bool> more =
                ext4->ReadDirectory(damage.directory, cookie, entry);
            if (!more)
            {
                error = more.error();
                break;
            }
            REQUIRE(*more);
        }
        REQUIRE(error == Error::Corrupt);
        REQUIRE(ErrorOf(ext4->Lookup(damage.directory, "missing")) == Error::Corrupt);
    }
#endif
}

TEST_CASE("inodes of ext4 groups never written read as free, whatever their table holds")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    using namespace FeatherDisk::Ext;
    std::vector<std::uint8_t> bytes = LoadExtImage("ext4-gdtcsum");
    RawVolume raw{bytes};
    const Superblock superblock = SuperblockOf(bytes);
    const std::uint32_t per_group = superblock.inodes_per_group;
    REQUIRE((DescriptorOf(raw, 1).flags & GroupInodesUnused) != 0);
    const GroupDescriptor first = DescriptorOf(raw, 0);
    REQUIRE(first.unused_inodes > 0);
    // A plausible file in group 1's first slot and in group 0's last, which was never used.
    const auto plant = [&raw](std::uint32_t inode)
    {
        const std::uint64_t offset = raw.InodeOffset(inode);
        raw.Store16(offset, 0100644);
        raw.Store32(offset + 4, 5);
        raw.Store16(offset + 26, 1);
    };
    const std::uint32_t in_group_one = per_group + 1;
    const std::uint32_t last_of_group_zero = per_group;
    plant(in_group_one);
    plant(last_of_group_zero);
    {
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
        MountedFilesystem ext4(Ext2Type(), *device, true);
        REQUIRE(ErrorOf(ext4->GetAttributes(in_group_one)) == Error::NotFound);
        REQUIRE(ErrorOf(ext4->GetAttributes(last_of_group_zero)) == Error::NotFound);
    }

    // With the flag and the count cleared (and the CRC16 made to match), the same slots are
    // read as inodes.
    const std::uint64_t group_one = raw.DescriptorOffset() + raw.DescriptorSize();
    raw.Store16(group_one + 18, 0);
    raw.Store16(group_one + 28, 0);
    raw.Store16(raw.DescriptorOffset() + 28, 0);
    for (const std::uint32_t group : {0U, 1U})
    {
        const std::uint64_t offset = raw.DescriptorOffset() + group * raw.DescriptorSize();
        raw.Store16(offset + 30, GroupDescriptorChecksum(
                                     superblock, VolumeChecksumSeed(superblock), group,
                                     std::span(bytes).subspan(offset, raw.DescriptorSize())));
    }
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
    MountedFilesystem ext4(Ext2Type(), *device, true);
    REQUIRE(Value(ext4->GetAttributes(in_group_one)).size == 5);
    REQUIRE(Value(ext4->GetAttributes(last_of_group_zero)).size == 5);
#endif
}

TEST_CASE("ext4 files flagged for extents on a volume without them are damage, not block maps")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    std::vector<std::uint8_t> bytes = LoadExtImage("ext4-blockmap");
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
    // debugfs keeps the inode's checksum right.
    REQUIRE(
        FeatherDisk::Tests::ChangeWithDebugfs(*device, "extent-flag", "sif /big.txt flags 0x80000")
            .status == 0);
    MountedFilesystem ext4(Ext2Type(), *device, true);
    const NodeId big = Value(Walk(*ext4, "big.txt"));
    REQUIRE(Value(ext4->GetAttributes(big)).size == 448000);
    std::array<std::uint8_t, 16> buffer{};
    REQUIRE(ErrorOf(ext4->Read(big, 0, buffer)) == Error::Corrupt);
#endif
}
