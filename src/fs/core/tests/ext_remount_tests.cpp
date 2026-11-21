// SPDX-License-Identifier: MIT OR Apache-2.0
// Filesystem::MakeReadOnly on ext2, ext3 and ext4: a mounted volume that is made read-only in
// place must be clean on disk, keep serving reads, and write nothing from then on.
#include <catch2/catch_test_macros.hpp>

#include "ext/ext2_filesystem.h"
#include "fs/ext2.h"
#include "tests/ext2_image.h"
#include "tests/filesystem_helpers.h"
#include "tests/memory_device.h"
#include "tests/page_cache_support.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace FeatherDisk;
using namespace FeatherDisk::Tests;
using Ext::Ext2Filesystem;
using Ext::Ext2Type;
using Ext::JournalActivity;

constexpr std::uint64_t SuperblockStart = 1024;
constexpr std::uint32_t NeedsRecovery = 0x4;
constexpr std::uint16_t CleanState = 0x1;

struct Variant
{
    std::string_view image;
    bool journal;
};

constexpr Variant Variants[] = {
    {"1k", false},
    {"ext3", true},
    {"ext4", true},
    {"ext4-4k", true},
};

Ext2Filesystem& Core(Filesystem& filesystem)
{
    return static_cast<Ext2Filesystem&>(filesystem);
}

JournalActivity ActivityOf(Filesystem& filesystem)
{
    const std::optional<JournalActivity> activity = Core(filesystem).Activity();
    REQUIRE(activity.has_value());
    return *activity;
}

bool NeedsRecoveryOn(MemoryDevice& device)
{
    std::vector<std::uint8_t> bytes = BytesOf(device);
    return (RawVolume{bytes}.Load32(SuperblockStart + 96) & NeedsRecovery) != 0;
}

bool MarkedClean(MemoryDevice& device)
{
    std::vector<std::uint8_t> bytes = BytesOf(device);
    return (RawVolume{bytes}.Load16(SuperblockStart + 58) & CleanState) != 0;
}

std::string CatOf(MemoryDevice& device, std::string_view path)
{
    return RunDebugfs(device, "remount-cat", "cat " + std::string(path)).output;
}

Filesystem* MountWritable(MemoryDevice& device)
{
    Filesystem* filesystem = Value(Ext2Type().Mount(Mountable(device), {.read_only = false}));
    REQUIRE(filesystem->GetCapabilities().writable);
    return filesystem;
}

constexpr std::string_view AlphaText = "alpha\n";

std::string BigText()
{
    return Pattern(12000);
}

void WriteTestFiles(Filesystem& filesystem)
{
    WriteNewFile(filesystem, "a.txt", std::string(AlphaText));
    WriteNewFile(filesystem, "big.bin", BigText());
}

} // namespace

TEST_CASE("MakeReadOnly leaves a written volume clean while it stays mounted")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const Variant& variant : Variants)
    {
        INFO(variant.image);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(variant.image));
        Filesystem* filesystem = MountWritable(*device);
        WriteTestFiles(*filesystem);
        const Tree before = TreeOf(*filesystem);

        Succeeds(filesystem->MakeReadOnly());
        REQUIRE_FALSE(filesystem->GetCapabilities().writable);
        REQUIRE_FALSE(NeedsRecoveryOn(*device));
        REQUIRE(MarkedClean(*device));
        const std::unique_ptr<MemoryDevice> copy = CopyOf(*device);
        CheckClean(*copy, "remount-t1");
        if (HaveE2fsck())
        {
            REQUIRE(CatOf(*copy, "/a.txt") == AlphaText);
            REQUIRE(CatOf(*copy, "/big.bin") == BigText());
        }

        const std::string difference = TreeDifference(TreeOf(*filesystem), before);
        INFO(difference);
        REQUIRE(difference.empty());
        REQUIRE(ReadAll(*filesystem, Value(Walk(*filesystem, "a.txt"))) == AlphaText);
        REQUIRE(ReadAll(*filesystem, Value(Walk(*filesystem, "big.bin"))) == BigText());
        const NodeId root = filesystem->Root();
        REQUIRE(ErrorOf(filesystem->Create(root, "late.txt", 0644)) == Error::ReadOnly);
        const NodeId file = Value(Walk(*filesystem, "a.txt"));
        REQUIRE(ErrorOf(filesystem->Write(file, 0, BytesOfText("x"))) == Error::ReadOnly);
        REQUIRE(filesystem->Remove(root, "a.txt").error() == Error::ReadOnly);

        const std::uint64_t writes = device->write_count;
        Succeeds(filesystem->Sync());
        Succeeds(filesystem->MakeReadOnly());
        REQUIRE(ReadAll(*filesystem, Value(Walk(*filesystem, "big.bin"))) == BigText());
        REQUIRE(device->write_count == writes);
        Succeeds(Ext2Type().Unmount(*filesystem));
        REQUIRE(device->write_count == writes);
        REQUIRE_FALSE(NeedsRecoveryOn(*device));
        REQUIRE(MarkedClean(*device));
        CheckClean(*device, "remount-t1-unmounted");
    }
#endif
}

TEST_CASE("a read-only remount then a writable mount replays nothing and keeps both changes")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const Variant& variant : Variants)
    {
        INFO(variant.image);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(variant.image));
        Filesystem* filesystem = MountWritable(*device);
        WriteNewFile(*filesystem, "a.txt", std::string(AlphaText));
        Succeeds(filesystem->MakeReadOnly());
        Succeeds(Ext2Type().Unmount(*filesystem));
        if (variant.journal)
        {
            REQUIRE(JournalSuperblockOf(*device).start == 0);
        }

        filesystem = MountWritable(*device);
        if (variant.journal)
        {
            REQUIRE(ActivityOf(*filesystem).transactions == 0);
        }
        WriteNewFile(*filesystem, "b.txt", "bravo\n");
        Succeeds(filesystem->MakeReadOnly());
        const std::unique_ptr<MemoryDevice> copy = CopyOf(*device);
        CheckClean(*copy, "remount-t2");
        if (HaveE2fsck())
        {
            REQUIRE(CatOf(*copy, "/a.txt") == AlphaText);
            REQUIRE(CatOf(*copy, "/b.txt") == "bravo\n");
        }
        Succeeds(Ext2Type().Unmount(*filesystem));
    }
#endif
}

namespace
{

struct Stopped
{
    std::unique_ptr<MemoryDevice> device;
    std::uint64_t writes_made;
};

// The volume with a.txt and big.bin written and not synced, then MakeReadOnly with the device
// stopping after `allowed` of its writes; allowed = UINT64_MAX lets it finish.
Stopped StopDuringMakeReadOnly(const std::vector<std::uint8_t>& image, std::uint64_t allowed,
                               bool lose_unflushed)
{
    std::unique_ptr<MemoryDevice> device = DeviceFrom(image);
    Filesystem* filesystem = MountWritable(*device);
    WriteTestFiles(*filesystem);
    device->TrackUnflushed();
    const std::uint64_t start = device->write_count;
    if (allowed != UINT64_MAX)
    {
        device->FailWritesAfter(start + allowed);
    }
    const Status status = filesystem->MakeReadOnly();
    Stopped stopped;
    stopped.writes_made = device->write_count - start;
    if (allowed != UINT64_MAX && stopped.writes_made >= allowed)
    {
        REQUIRE_FALSE(status.has_value());
    }
    stopped.device =
        lose_unflushed ? device->CopyKeeping([](std::size_t) { return false; }) : CopyOf(*device);
    if (!Ext2Type().Unmount(*filesystem))
    {
        Succeeds(Ext2Type().Unmount(*filesystem));
    }
    return stopped;
}

void RequireWholeOrAbsent(Filesystem& filesystem)
{
    const Result<NodeId> a = Walk(filesystem, "a.txt");
    if (a)
    {
        REQUIRE(ReadAll(filesystem, *a) == AlphaText);
    }
    const Result<NodeId> big = Walk(filesystem, "big.bin");
    if (big)
    {
        REQUIRE(ReadAll(filesystem, *big) == BigText());
    }
}

} // namespace

TEST_CASE("a stop anywhere inside MakeReadOnly leaves a volume that recovers")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const std::string_view name : {"ext3", "ext4"})
    {
        INFO(name);
        const std::vector<std::uint8_t> image = LoadExtImage(name);
        const Stopped whole = StopDuringMakeReadOnly(image, UINT64_MAX, false);
        const std::uint64_t total = whole.writes_made;
        REQUIRE(total >= 3);
        REQUIRE_FALSE(NeedsRecoveryOn(*whole.device));
        CheckClean(*whole.device, "remount-t3-whole");

        for (std::uint64_t allowed = 0; allowed < total; ++allowed)
        {
            INFO("stopped after write " << allowed << " of " << total);
            for (const bool lose_unflushed : {false, true})
            {
                INFO("unflushed writes lost: " << lose_unflushed);
                Stopped stopped = StopDuringMakeReadOnly(image, allowed, lose_unflushed);
                ReplayByMounting(*stopped.device);
                {
                    MountedFilesystem recovered(Ext2Type(), *stopped.device, true);
                    RequireWholeOrAbsent(*recovered);
                }
                CheckClean(*stopped.device, "remount-t3");
            }
        }
    }
#endif
}

TEST_CASE("an aborted journal refuses writes and MakeReadOnly then writes nothing")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage("ext4"));
    Filesystem* filesystem = MountWritable(*device);
    WriteNewFile(*filesystem, "a.txt", std::string(AlphaText));
    Succeeds(filesystem->Sync());
    WriteNewFile(*filesystem, "b.txt", "bravo\n");
    device->FailWritesAfter(device->write_count);
    REQUIRE_FALSE(filesystem->Sync().has_value());
    REQUIRE_FALSE(filesystem->GetCapabilities().writable);
    REQUIRE(ActivityOf(*filesystem).aborted);

    device->FailWritesAfter(UINT64_MAX);
    const std::uint64_t writes = device->write_count;
    Succeeds(filesystem->MakeReadOnly());
    Succeeds(filesystem->MakeReadOnly());
    REQUIRE(device->write_count == writes);
    REQUIRE(NeedsRecoveryOn(*device));
    REQUIRE(ErrorOf(filesystem->Create(filesystem->Root(), "c.txt", 0644)) == Error::ReadOnly);
    Succeeds(Ext2Type().Unmount(*filesystem));
    REQUIRE(device->write_count == writes);
    REQUIRE(NeedsRecoveryOn(*device));

    const std::unique_ptr<MemoryDevice> copy = CopyOf(*device);
    {
        MountedFilesystem replayed(Ext2Type(), *copy);
        REQUIRE(replayed->GetCapabilities().writable);
        REQUIRE(ReadAll(*replayed, Value(Walk(*replayed, "a.txt"))) == AlphaText);
        REQUIRE_FALSE(Walk(*replayed, "b.txt").has_value());
    }
    CheckClean(*copy, "remount-t4");
#endif
}

TEST_CASE("MakeReadOnly on an instance that writes nothing writes nothing")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const Variant& variant : Variants)
    {
        INFO(variant.image);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(variant.image));
        const std::vector<std::uint8_t> before = BytesOf(*device);
        {
            MountedFilesystem filesystem(Ext2Type(), *device, true);
            REQUIRE_FALSE(filesystem->GetCapabilities().writable);
            Succeeds(filesystem->MakeReadOnly());
            Succeeds(filesystem->MakeReadOnly());
            REQUIRE_FALSE(filesystem->GetCapabilities().writable);
            REQUIRE(ErrorOf(filesystem->Create(filesystem->Root(), "x", 0644)) == Error::ReadOnly);
        }
        REQUIRE(device->write_count == 0);
        REQUIRE(BytesOf(*device) == before);
    }

    // An ext2 volume that was not cleanly unmounted falls back to read-only by itself.
    std::vector<std::uint8_t> unclean = LoadExtImage("1k");
    RawVolume{unclean}.Store16(SuperblockStart + 58, 0);
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(unclean);
    {
        MountedFilesystem filesystem(Ext2Type(), *device);
        REQUIRE_FALSE(filesystem->GetCapabilities().writable);
        Succeeds(filesystem->MakeReadOnly());
        REQUIRE_FALSE(filesystem->GetCapabilities().writable);
    }
    REQUIRE(device->write_count == 0);
    REQUIRE(BytesOf(*device) == unclean);
#endif
}
