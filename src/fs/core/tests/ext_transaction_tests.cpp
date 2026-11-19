// SPDX-License-Identifier: MIT OR Apache-2.0
// jbd2 transactions on writable ext3 and ext4 volumes: which journals mount writable, what a commit
// puts in the log, the order and limits of commits and checkpoints, and what a change that does not
// fit, or a failing device, leaves behind. Every volume the core wrote is checked by e2fsck, and
// the logs by debugfs's logdump and journal replay.
#include <catch2/catch_test_macros.hpp>

#include "ext/ext2_filesystem.h"
#include "ext/journal.h"
#include "ext/superblock.h"
#include "ext/transaction.h"
#include "fs/ext2.h"
#include "tests/ext2_image.h"
#include "tests/filesystem_helpers.h"
#include "tests/memory_device.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using namespace FeatherDisk;
using namespace FeatherDisk::Tests;
using Ext::BlockLayer;
using Ext::BlockSet;
using Ext::DecodeGroupDescriptor;
using Ext::DecodeJournalSuperblock;
using Ext::Ext2Filesystem;
using Ext::Ext2Type;
using Ext::GroupDescriptor;
using Ext::JournalActivity;
using Ext::JournalLimits;
using Ext::JournalMagic;
using Ext::JournalSuperblock;

constexpr std::uint64_t SuperblockStart = 1024;
constexpr std::uint32_t NeedsRecovery = 0x4;
constexpr std::uint16_t CleanState = 0x1;
constexpr std::uint16_t ErrorsState = 0x2;
constexpr std::uint32_t Revoke = Ext::JournalIncompatibleRevoke;
constexpr std::uint32_t Wide = Ext::JournalIncompatible64Bit;
constexpr std::uint32_t Async = Ext::JournalIncompatibleAsyncCommit;
constexpr std::uint32_t V2 = Ext::JournalIncompatibleChecksumV2;
constexpr std::uint32_t V3 = Ext::JournalIncompatibleChecksumV3;
// Leaves a limit as it is: LimitJournal only lowers.
constexpr std::uint32_t Unlimited = 0xFFFFFFFF;

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

// A mount that can be ended on purpose, to see what unmounting says.
class Mounted
{
public:
    explicit Mounted(MemoryDevice& volume, bool read_only = false)
    {
        filesystem = Value(Ext2Type().Mount(volume, {.read_only = read_only}));
    }
    ~Mounted()
    {
        if (filesystem != nullptr)
        {
            static_cast<void>(Ext2Type().Unmount(*filesystem));
        }
    }
    Mounted(const Mounted&) = delete;
    Mounted& operator=(const Mounted&) = delete;

    Filesystem& operator*()
    {
        return *filesystem;
    }
    Filesystem* operator->()
    {
        return filesystem;
    }

    Status Unmount()
    {
        const Status status = Ext2Type().Unmount(*filesystem);
        if (status)
        {
            filesystem = nullptr;
        }
        return status;
    }

private:
    Filesystem* filesystem = nullptr;
};

// The volume's journal superblock, where it lies in the bytes, and its fields as raw big-endian.
std::uint64_t JournalBase(std::vector<std::uint8_t>& bytes)
{
    const RawVolume raw{bytes};
    return JournalOffset(raw, 0);
}

void SetJournalField(std::vector<std::uint8_t>& bytes, std::uint32_t offset, std::uint32_t value)
{
    Ext::StoreBigEndian32(bytes.data() + JournalBase(bytes) + offset, value);
}

constexpr std::uint32_t TypeField = 4;
constexpr std::uint32_t LengthField = 0x10;
constexpr std::uint32_t StartField = 0x1C;

void SetFeatures(std::vector<std::uint8_t>& bytes, std::uint32_t compatible,
                 std::uint32_t incompatible)
{
    const std::uint64_t base = JournalBase(bytes);
    const std::span<std::uint8_t> block = std::span(bytes).subspan(base, 1024);
    JournalSuperblock journal = DecodeJournalSuperblock(block);
    journal.compatible = compatible;
    journal.incompatible = incompatible;
    Ext::EncodeJournalFeatures(journal, block);
}

void UpdateSuperblock(std::vector<std::uint8_t>& bytes)
{
    RawVolume raw{bytes};
    if ((raw.Load32(SuperblockStart + 100) & 0x400U) != 0)
    {
        raw.UpdateSuperblockChecksum();
    }
}

std::uint32_t BlockSizeOf(const std::vector<std::uint8_t>& bytes)
{
    return RawVolume{const_cast<std::vector<std::uint8_t>&>(bytes)}.BlockSize();
}

struct LoggedBlock
{
    std::uint64_t block;
    std::uint32_t flags;
    std::uint64_t journal_block;
};

std::vector<LoggedBlock> LoggedBlocks(const std::string& dump)
{
    std::vector<LoggedBlock> blocks;
    std::size_t at = 0;
    while (at < dump.size())
    {
        std::size_t end = dump.find('\n', at);
        end = end == std::string::npos ? dump.size() : end;
        const std::string line = dump.substr(at, end - at);
        at = end + 1;
        unsigned long long block = 0;
        unsigned long long journal_block = 0;
        unsigned flags = 0;
        if (std::sscanf(line.c_str(), " FS block %llu logged at journal block %llu (flags 0x%x)",
                        &block, &journal_block, &flags) == 3)
        {
            blocks.push_back({block, flags, journal_block});
        }
    }
    return blocks;
}

std::set<std::uint64_t> LoggedSet(const std::string& dump)
{
    std::set<std::uint64_t> set;
    for (const LoggedBlock& block : LoggedBlocks(dump))
    {
        set.insert(block.block);
    }
    return set;
}

std::set<std::uint64_t> RevokedSet(const std::string& dump)
{
    std::set<std::uint64_t> set;
    std::size_t at = 0;
    const std::string marker = "Revoke FS block ";
    while ((at = dump.find(marker, at)) != std::string::npos)
    {
        at += marker.size();
        set.insert(std::stoull(dump.substr(at)));
    }
    return set;
}

std::size_t CountOf(const std::string& text, std::string_view needle)
{
    std::size_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + 1))
    {
        ++count;
    }
    return count;
}

std::string Logdump(MemoryDevice& device)
{
    return RunDebugfs(device, "transaction-logdump", "logdump -a").output;
}

// Both ways of recovering a crashed volume leave the same bytes (ignoring what a mount writes
// about itself), and e2fsck finds nothing wrong with either. The bytes are returned.
std::vector<std::uint8_t> BothReplays(MemoryDevice& crashed, std::string_view name)
{
    const std::unique_ptr<MemoryDevice> theirs = CopyOf(crashed);
    const std::unique_ptr<MemoryDevice> ours = CopyOf(crashed);
    REQUIRE(ChangeWithDebugfs(*theirs, std::string(name) + "-debugfs", "jr").status == 0);
    ReplayByMounting(*ours);
    RequireSameAsDebugfs(*ours, *theirs);
    CheckClean(*ours, std::string(name) + "-replayed");
    CheckClean(*theirs, std::string(name) + "-debugfs");
    return BytesOf(*ours);
}

// What a crash leaves after the journal is replayed.
std::vector<std::uint8_t> ReplayedBytes(MemoryDevice& crashed)
{
    const std::unique_ptr<MemoryDevice> copy = CopyOf(crashed);
    ReplayByMounting(*copy);
    return BytesOf(*copy);
}

GroupDescriptor GroupOf(const std::vector<std::uint8_t>& bytes, std::uint32_t group)
{
    const RawVolume raw{const_cast<std::vector<std::uint8_t>&>(bytes)};
    return DecodeGroupDescriptor(std::span(bytes).subspan(
        raw.DescriptorOffset() + std::uint64_t{group} * raw.DescriptorSize(),
        raw.DescriptorSize()));
}

std::uint32_t GroupOfInode(const std::vector<std::uint8_t>& bytes, std::uint32_t inode)
{
    const RawVolume raw{const_cast<std::vector<std::uint8_t>&>(bytes)};
    return (inode - 1) / raw.Load32(SuperblockStart + 40);
}

std::uint32_t GroupOfBlock(const std::vector<std::uint8_t>& bytes, std::uint64_t block)
{
    const RawVolume raw{const_cast<std::vector<std::uint8_t>&>(bytes)};
    return static_cast<std::uint32_t>((block - raw.Load32(SuperblockStart + 20)) /
                                      raw.Load32(SuperblockStart + 32));
}

std::uint64_t InodeTableBlock(const std::vector<std::uint8_t>& bytes, std::uint32_t inode)
{
    const RawVolume raw{const_cast<std::vector<std::uint8_t>&>(bytes)};
    return raw.InodeOffset(inode) / raw.BlockSize();
}

std::vector<std::uint64_t> BlocksOf(const std::vector<std::uint8_t>& bytes, std::uint32_t inode,
                                    std::uint32_t count)
{
    const RawVolume raw{const_cast<std::vector<std::uint8_t>&>(bytes)};
    std::vector<std::uint64_t> blocks;
    for (std::uint32_t logical = 0; logical < count; ++logical)
    {
        blocks.push_back(raw.FileBlock(inode, logical));
    }
    return blocks;
}

bool BlockInUse(const std::vector<std::uint8_t>& bytes, std::uint64_t block)
{
    const RawVolume raw{const_cast<std::vector<std::uint8_t>&>(bytes)};
    const std::uint32_t group = GroupOfBlock(bytes, block);
    const std::uint64_t bit =
        (block - raw.Load32(SuperblockStart + 20)) % raw.Load32(SuperblockStart + 32);
    const std::uint64_t at =
        raw.BlockOffset(static_cast<std::uint32_t>(GroupOf(bytes, group).block_bitmap)) + bit / 8;
    return ((bytes[at] >> (bit % 8)) & 1U) != 0;
}

bool SameBlock(const std::vector<std::uint8_t>& left, const std::vector<std::uint8_t>& right,
               std::uint64_t block, std::uint32_t block_size)
{
    return std::equal(left.begin() + static_cast<std::ptrdiff_t>(block * block_size),
                      left.begin() + static_cast<std::ptrdiff_t>((block + 1) * block_size),
                      right.begin() + static_cast<std::ptrdiff_t>(block * block_size));
}

// What Pattern(offset + length) holds from offset on.
std::string PatternAt(std::uint64_t offset, std::size_t length)
{
    std::string text(length, '\0');
    for (std::size_t index = 0; index < length; ++index)
    {
        text[index] = static_cast<char>('a' + (offset + index) % 23);
    }
    return text;
}

// Writes PatternAt contents until the volume has no free block, one block at a time at the end;
// returns the file's size.
std::uint64_t FillVolume(Filesystem& filesystem, NodeId file)
{
    std::uint64_t offset = 0;
    for (const std::size_t size : {std::size_t{64 * 1024}, std::size_t{1024}})
    {
        for (;;)
        {
            const std::string chunk = PatternAt(offset, size);
            const Result<std::size_t> written = filesystem.Write(file, offset, BytesOfText(chunk));
            if (!written || *written == 0)
            {
                break;
            }
            offset += *written;
        }
    }
    REQUIRE(FreeBlocks(filesystem) == 0);
    return offset;
}

std::vector<std::string> NamesIn(Filesystem& filesystem, std::string_view path)
{
    return Sorted(List(filesystem, Value(Walk(filesystem, path))));
}

} // namespace

TEST_CASE("block layers hold, replace, drop and absorb block copies")
{
    constexpr std::uint32_t Size = 1024;
    BlockLayer layer(Size);
    REQUIRE(layer.Count() == 0);
    REQUIRE(layer.Find(5) == nullptr);
    layer.Drop(5);
    for (std::uint64_t block = 0; block < 100; ++block)
    {
        std::uint8_t* copy = layer.Hold(block * 7 + 3);
        REQUIRE(copy != nullptr);
        std::fill(copy, copy + Size, static_cast<std::uint8_t>(block));
    }
    REQUIRE(layer.Count() == 100);
    REQUIRE(layer.Hold(10) == layer.Find(10));
    REQUIRE(layer.Count() == 100);
    // Dropping every third copy keeps the others whole, whatever the table's probing did.
    for (std::uint64_t block = 0; block < 100; block += 3)
    {
        layer.Drop(block * 7 + 3);
        layer.Drop(block * 7 + 3);
    }
    REQUIRE(layer.Count() == 66);
    for (std::uint64_t block = 0; block < 100; ++block)
    {
        const std::uint8_t* copy = layer.Find(block * 7 + 3);
        if (block % 3 == 0)
        {
            REQUIRE(copy == nullptr);
            continue;
        }
        REQUIRE(copy != nullptr);
        REQUIRE(copy[0] == static_cast<std::uint8_t>(block));
        REQUIRE(copy[Size - 1] == static_cast<std::uint8_t>(block));
    }
    std::size_t visited = 0;
    Succeeds(layer.ForEach(
        [&](std::uint64_t, const std::uint8_t*) -> Status
        {
            ++visited;
            return {};
        }));
    REQUIRE(visited == 66);

    BlockLayer other(Size);
    std::fill(other.Hold(10), other.Hold(10) + Size, std::uint8_t{0xEE});
    std::fill(other.Hold(1000000), other.Hold(1000000) + Size, std::uint8_t{0xDD});
    Succeeds(layer.Absorb(other));
    REQUIRE(other.Count() == 0);
    REQUIRE(layer.Count() == 67);
    REQUIRE(layer.Find(10)[17] == 0xEE);
    REQUIRE(layer.Find(1000000)[Size - 1] == 0xDD);
    layer.Clear();
    REQUIRE(layer.Count() == 0);
    REQUIRE(layer.Find(10) == nullptr);

    // Never more copies than the memory a layer may use.
    BlockLayer small(4096);
    REQUIRE(small.Maximum() == 1024);
    for (std::uint64_t block = 0; block < small.Maximum(); ++block)
    {
        REQUIRE(small.Hold(block) != nullptr);
    }
    REQUIRE(small.Hold(1024) == nullptr);
    REQUIRE(small.Count() == 1024);
}

TEST_CASE("block sets keep insertion order without repeats")
{
    BlockSet set;
    for (std::uint64_t value = 0; value < 200; ++value)
    {
        Succeeds(set.Add(value * 11));
        Succeeds(set.Add(value * 11));
    }
    REQUIRE(set.Count() == 200);
    REQUIRE(set.Contains(11 * 199));
    REQUIRE_FALSE(set.Contains(12));
    for (std::size_t index = 0; index < 200; ++index)
    {
        REQUIRE(set.At(index) == index * 11);
    }
    set.Clear();
    REQUIRE(set.Count() == 0);
    REQUIRE_FALSE(set.Contains(11));
}

#ifdef FEATHERDISK_EXT_IMAGES

TEST_CASE("ext3 and ext4 volumes with an internal journal mount writable, the others read-only")
{
    for (const std::string_view variant : {"ext3", "ext4", "ext4-blockmap", "ext4-64bit",
                                           "ext4-extents", "ext4-journal-gdtcsum", "ext4-4k"})
    {
        INFO(variant);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(variant));
        {
            Mounted ext(*device);
            REQUIRE(ext->GetCapabilities().writable);
            WriteNewFile(*ext, "t4.txt", "writable");
            Succeeds(ext.Unmount());
        }
        CheckClean(*device, "transaction-writable");
    }

    struct Case
    {
        std::string_view name;
        std::function<void(std::vector<std::uint8_t>&)> change;
    };
    const std::vector<Case> refusals = {
        {"journal on another device",
         [](std::vector<std::uint8_t>& bytes)
         {
             RawVolume raw{bytes};
             raw.Store32(SuperblockStart + 224, 0);
             UpdateSuperblock(bytes);
         }},
        {"version 1 journal",
         [](std::vector<std::uint8_t>& bytes) { SetJournalField(bytes, TypeField, 3); }},
        {"journal of 1000 blocks",
         [](std::vector<std::uint8_t>& bytes) { SetJournalField(bytes, LengthField, 1000); }},
        {"unknown journal feature (fast commit)",
         [](std::vector<std::uint8_t>& bytes) { SetFeatures(bytes, 0, 0x20); }},
        {"async commit without checksums",
         [](std::vector<std::uint8_t>& bytes) { SetFeatures(bytes, 0, Async); }},
        {"journal superblock without its magic",
         [](std::vector<std::uint8_t>& bytes) { SetJournalField(bytes, 0, 0); }},
        {"log not empty but needs_recovery clear",
         [](std::vector<std::uint8_t>& bytes) { SetJournalField(bytes, StartField, 5); }},
        {"fast commit feature on the volume",
         [](std::vector<std::uint8_t>& bytes)
         {
             RawVolume raw{bytes};
             raw.Store32(SuperblockStart + 92, raw.Load32(SuperblockStart + 92) | 0x400U);
             UpdateSuperblock(bytes);
         }},
        {"orphans left by another system (orphan_present)",
         [](std::vector<std::uint8_t>& bytes)
         {
             RawVolume raw{bytes};
             raw.Store32(SuperblockStart + 100, raw.Load32(SuperblockStart + 100) | 0x10000U);
             UpdateSuperblock(bytes);
         }},
        {"a free inode on the orphan list (s_last_orphan)",
         [](std::vector<std::uint8_t>& bytes)
         {
             const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
             const std::uint32_t last = RawVolume{bytes}.InodeCount();
             REQUIRE(ChangeWithDebugfs(*device, "transaction-last-orphan",
                                       "ssv last_orphan " + std::to_string(last))
                         .status == 0);
             bytes = BytesOf(*device);
             REQUIRE(RawVolume{bytes}.Load32(SuperblockStart + 232) == last);
         }},
        {"journal inode that is a directory",
         [](std::vector<std::uint8_t>& bytes)
         {
             RawVolume raw{bytes};
             raw.Store16(raw.InodeOffset(8), 040755);
         }},
    };
    for (const std::string_view variant : {"ext3", "ext4"})
    {
        for (const Case& refusal : refusals)
        {
            INFO(variant << ": " << refusal.name);
            // With the orphan file and nothing in it, the flag is cleared by a writable mount.
            if (variant == "ext4" && refusal.name.starts_with("orphans left by another system"))
            {
                continue;
            }
            std::vector<std::uint8_t> bytes = LoadExtImage(variant);
            refusal.change(bytes);
            const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
            {
                Mounted ext(*device);
                REQUIRE_FALSE(ext->GetCapabilities().writable);
                REQUIRE(ErrorOf(ext->Create(ext->Root(), "x", 0644)) == Error::ReadOnly);
                REQUIRE(NamesIn(*ext, "").size() > 5);
                Succeeds(ext.Unmount());
            }
            REQUIRE(device->write_count == 0);
            REQUIRE(device->flush_count == 0);
            RequireSameBytes(BytesOf(*device), bytes);
        }
        INFO(variant << ": a device that refuses writes");
        const std::vector<std::uint8_t> bytes = LoadExtImage(variant);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
        device->read_only = true;
        {
            Mounted ext(*device);
            REQUIRE_FALSE(ext->GetCapabilities().writable);
        }
        REQUIRE(device->write_count == 0);
        REQUIRE(device->flush_count == 0);
    }

    // async_commit needs checksums v2 or v3, and then a writable mount keeps it.
    std::vector<std::uint8_t> bytes = LoadExtImage("ext4");
    SetFeatures(bytes, 0, Async | V3);
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
    {
        Mounted ext(*device);
        REQUIRE(ext->GetCapabilities().writable);
    }
    CheckClean(*device, "transaction-async");
}

TEST_CASE("a writable mount marks the journal in use and the first transaction follows the log")
{
    for (const std::string_view variant :
         {"ext3", "ext4", "ext4-4k", "ext4-64bit", "ext4-journal-gdtcsum"})
    {
        INFO(variant);
        std::vector<std::uint8_t> original = LoadExtImage(variant);
        const bool checksums = (RawVolume{original}.Load32(SuperblockStart + 100) & 0x400U) != 0;
        const bool wide = (RawVolume{original}.Load32(SuperblockStart + 96) & 0x80U) != 0;
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(original);
        {
            Mounted ext(*device);
            std::vector<std::uint8_t> bytes = BytesOf(*device);
            RawVolume raw{bytes};
            REQUIRE((raw.Load32(SuperblockStart + 96) & NeedsRecovery) != 0);
            REQUIRE((raw.Load16(SuperblockStart + 58) & CleanState) != 0);
            const JournalSuperblock journal = JournalSuperblockOf(*device);
            REQUIRE((journal.incompatible & Revoke) != 0);
            REQUIRE((journal.incompatible & Wide) == (wide ? Wide : 0));
            REQUIRE((journal.incompatible & V3) == (checksums ? V3 : 0));
            REQUIRE((journal.incompatible & V2) == 0);
            REQUIRE((journal.compatible & Ext::JournalCompatibleChecksum) == 0);
            REQUIRE(journal.checksum_matches);
            REQUIRE(journal.start == 0);
            REQUIRE(ActivityOf(*ext).next_sequence == journal.sequence + 1);

            WriteNewFile(*ext, "t5.txt", "one");
            Succeeds(ext->Sync());
            const JournalSuperblock after = JournalSuperblockOf(*device);
            REQUIRE(after.start != 0);
            REQUIRE(after.sequence == journal.sequence + 1);
            REQUIRE(ActivityOf(*ext).next_sequence == after.sequence + 1);
            REQUIRE(Logdump(*device).find("transaction " + std::to_string(after.sequence)) !=
                    std::string::npos);
            Succeeds(ext.Unmount());
        }
        std::vector<std::uint8_t> bytes = BytesOf(*device);
        RawVolume raw{bytes};
        REQUIRE((raw.Load32(SuperblockStart + 96) & NeedsRecovery) == 0);
        REQUIRE((raw.Load16(SuperblockStart + 58) & CleanState) != 0);
        REQUIRE(JournalSuperblockOf(*device).start == 0);
        CheckClean(*device, "transaction-state");
        MountedFilesystem again(Ext2Type(), *device, true);
        REQUIRE(ReadAll(*again, Value(Walk(*again, "t5.txt"))) == "one");
    }

    // A journal with checksums v2 keeps them, as the features of a journal that is already in use.
    std::vector<std::uint8_t> bytes = LoadExtImage("ext4");
    SetFeatures(bytes, Ext::JournalCompatibleChecksum, Revoke | V2);
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
    {
        Mounted ext(*device);
        const JournalSuperblock journal = JournalSuperblockOf(*device);
        REQUIRE((journal.incompatible & V2) != 0);
        REQUIRE((journal.incompatible & V3) == 0);
        REQUIRE((journal.incompatible & Wide) != 0);
        REQUIRE((journal.compatible & Ext::JournalCompatibleChecksum) == 0);
        REQUIRE(journal.checksum_matches);
    }
    CheckClean(*device, "transaction-v2");
}

TEST_CASE("a transaction holds the metadata of a change and no file data")
{
    for (const std::string_view variant : {"ext3", "ext4", "ext4-extents"})
    {
        INFO(variant);
        const std::vector<std::uint8_t> original = LoadExtImage(variant);
        const std::uint32_t block_size = BlockSizeOf(original);
        const std::string contents = Pattern(3 * block_size);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(original);
        std::unique_ptr<MemoryDevice> crashed;
        NodeId file = 0;
        {
            Mounted ext(*device);
            file = WriteNewFile(*ext, "a.txt", contents);
            const std::uint32_t flushes = device->flush_count;
            Succeeds(ext->Sync());
            REQUIRE(device->flush_count - flushes == 2);
            REQUIRE(ActivityOf(*ext).transactions == 1);
            crashed = CopyOf(*device);
        }
        const std::vector<std::uint8_t> recovered = ReplayedBytes(*crashed);
        const auto inode = static_cast<std::uint32_t>(file);
        const std::vector<std::uint64_t> data = BlocksOf(recovered, inode, 3);
        const RawVolume raw{const_cast<std::vector<std::uint8_t>&>(recovered)};

        const std::uint32_t first_data = raw.Load32(SuperblockStart + 20);
        std::set<std::uint64_t> expected = {block_size == 1024 ? 1U : 0U, first_data + 1U};
        expected.insert(GroupOf(recovered, GroupOfBlock(recovered, data[0])).block_bitmap);
        expected.insert(GroupOf(recovered, GroupOfInode(recovered, inode)).inode_bitmap);
        expected.insert(InodeTableBlock(recovered, inode));
        expected.insert(InodeTableBlock(recovered, 2));
        expected.insert(raw.FileBlock(2, 0));
        const std::string dump = Logdump(*crashed);
        INFO(dump);
        REQUIRE(LoggedSet(dump) == expected);
        for (const std::uint64_t block : data)
        {
            REQUIRE(expected.count(block) == 0);
        }

        // The data is where the file says; nothing else of the change has reached its place.
        const std::vector<std::uint8_t> on_disk = BytesOf(*crashed);
        for (std::uint32_t index = 0; index < 3; ++index)
        {
            REQUIRE(std::equal(
                contents.begin() + index * block_size, contents.begin() + (index + 1) * block_size,
                on_disk.begin() + static_cast<std::ptrdiff_t>(data[index] * block_size)));
        }
        for (const std::uint64_t block : expected)
        {
            if (block != (block_size == 1024 ? 1U : 0U))
            {
                REQUIRE(SameBlock(on_disk, original, block, block_size));
            }
        }
        BothReplays(*crashed, "transaction-content");
    }
}

TEST_CASE("changes are read back at once but reach their places only with the commit")
{
    for (const std::string_view variant : {"ext3", "ext4", "ext4-extents"})
    {
        INFO(variant);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(variant));
        Mounted ext(*device);
        const std::vector<std::uint8_t> mounted = BytesOf(*device);
        const std::uint32_t block_size = BlockSizeOf(mounted);
        const NodeId root = ext->Root();
        const NodeId hello = Value(Walk(*ext, "hello.txt"));
        const NodeId big = Value(Walk(*ext, "big.txt"));
        const NodeId directory = Value(ext->MakeDirectory(root, "d", 0755));
        const NodeId file = WriteNewFile(*ext, "d/f.txt", Pattern(2500));
        Succeeds(ext->Rename(directory, "f.txt", directory, "g.txt"));
        const std::string target = "a-slow-link-target-" + std::string(80, 'x');
        const NodeId link = Value(ext->SymbolicLink(directory, "s", target));
        Succeeds(ext->Remove(root, "hello.txt"));
        Succeeds(ext->Truncate(big, 1000));

        REQUIRE(NamesIn(*ext, "d") == std::vector<std::string>{"g.txt", "s"});
        REQUIRE(ErrorOf(ext->Lookup(root, "hello.txt")) == Error::NotFound);
        REQUIRE(Value(ext->Lookup(directory, "g.txt")) == file);
        REQUIRE(ReadAll(*ext, file) == Pattern(2500));
        std::array<char, 200> buffer{};
        const std::size_t length = Value(ext->ReadLink(link, buffer));
        REQUIRE(std::string(buffer.data(), length) == target);
        REQUIRE(Value(ext->GetAttributes(link)).size == target.size());
        REQUIRE(Value(ext->GetAttributes(big)).size == 1000);
        REQUIRE(ReadAll(*ext, big) == ReadAll(*ext, big).substr(0, 1000));
        const VolumeStatistics statistics = Value(ext->Statistics());

        const std::vector<std::uint8_t> before_sync = BytesOf(*device);
        Succeeds(ext->Sync());
        const std::unique_ptr<MemoryDevice> crashed = CopyOf(*device);
        const std::vector<std::uint8_t> recovered = ReplayedBytes(*crashed);

        // Nothing of the change but file data was in its place before the commit.
        std::set<std::uint64_t> homes = {
            RawVolume{const_cast<std::vector<std::uint8_t>&>(recovered)}.Load32(SuperblockStart +
                                                                                20) +
                1U,
            block_size == 1024 ? 1U : 0U};
        const RawVolume raw{const_cast<std::vector<std::uint8_t>&>(recovered)};
        for (const NodeId node : {root, hello, big, directory, file, link})
        {
            const auto inode = static_cast<std::uint32_t>(node);
            homes.insert(InodeTableBlock(recovered, inode));
            homes.insert(GroupOf(recovered, GroupOfInode(recovered, inode)).inode_bitmap);
        }
        for (const std::uint64_t block : BlocksOf(recovered, static_cast<std::uint32_t>(file), 3))
        {
            homes.insert(GroupOf(recovered, GroupOfBlock(recovered, block)).block_bitmap);
        }
        homes.insert(raw.FileBlock(2, 0));
        homes.insert(raw.FileBlock(static_cast<std::uint32_t>(directory), 0));
        homes.insert(raw.FileBlock(static_cast<std::uint32_t>(link), 0));
        for (const std::uint64_t block : homes)
        {
            INFO("block " << block);
            REQUIRE(SameBlock(before_sync, mounted, block, block_size));
        }

        // What the crash leaves is what the live volume showed.
        const std::unique_ptr<MemoryDevice> after = DeviceFrom(recovered);
        MountedFilesystem again(Ext2Type(), *after, true);
        const VolumeStatistics seen = Value(again->Statistics());
        REQUIRE(seen.free_blocks == statistics.free_blocks);
        REQUIRE(seen.free_nodes == statistics.free_nodes);
        REQUIRE(NamesIn(*again, "d") == std::vector<std::string>{"g.txt", "s"});
        REQUIRE(ReadAll(*again, Value(Walk(*again, "d/g.txt"))) == Pattern(2500));
        REQUIRE(ErrorOf(Walk(*again, "hello.txt")) == Error::NotFound);
        REQUIRE(Value(again->GetAttributes(Value(Walk(*again, "big.txt")))).size == 1000);
    }
}

TEST_CASE("a block that begins with the journal's magic is logged escaped and replays whole")
{
    const std::string magic("\xC0\x3B\x39\x98", 4);
    struct Case
    {
        std::string_view image;
        bool version2;
    };
    // The superblock's block is block 0 with 2 and 4 KiB blocks, and begins in the boot sector.
    for (const Case& which :
         {Case{"ext4-64bit", false}, Case{"ext4-4k", false}, Case{"ext4-64bit", true}})
    {
        INFO(which.image << (which.version2 ? " with checksums v2" : ""));
        std::vector<std::uint8_t> bytes = LoadExtImage(which.image);
        std::copy(magic.begin(), magic.end(), bytes.begin());
        if (which.version2)
        {
            SetFeatures(bytes, 0, Revoke | Wide | V2);
        }
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
        std::unique_ptr<MemoryDevice> crashed;
        {
            Mounted ext(*device);
            WriteNewFile(*ext, "esc.txt", "escaped");
            Succeeds(ext->Sync());
            crashed = CopyOf(*device);
        }
        const std::string dump = Logdump(*crashed);
        INFO(dump);
        const std::vector<LoggedBlock> logged = LoggedBlocks(dump);
        const auto first = std::find_if(logged.begin(), logged.end(),
                                        [](const LoggedBlock& entry) { return entry.block == 0; });
        REQUIRE(first != logged.end());
        REQUIRE((first->flags & Ext::JournalTagEscaped) != 0);
        for (const LoggedBlock& entry : logged)
        {
            REQUIRE(((entry.flags & Ext::JournalTagEscaped) != 0) == (entry.block == 0));
        }
        std::vector<std::uint8_t> on_disk = BytesOf(*crashed);
        const RawVolume raw{on_disk};
        const std::uint64_t copy =
            JournalOffset(raw, static_cast<std::uint32_t>(first->journal_block));
        REQUIRE(Ext::LoadBigEndian32(on_disk.data() + copy) == 0);
        const std::vector<std::uint8_t> replayed = BothReplays(*crashed, "transaction-escape");
        REQUIRE(Ext::LoadBigEndian32(replayed.data()) == JournalMagic);
        // The rest of the block is what the log held.
        REQUIRE(std::equal(on_disk.begin() + static_cast<std::ptrdiff_t>(copy + 4),
                           on_disk.begin() + static_cast<std::ptrdiff_t>(copy + 1024),
                           replayed.begin() + 4));
    }

    // With 1 KiB blocks no journalled block begins with the magic of itself; a long symbolic link's
    // block does when its target does.
    for (const std::string_view image : {"ext3", "ext4"})
    {
        INFO(image);
        const std::string target = magic + std::string(80, 'x');
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(image));
        std::unique_ptr<MemoryDevice> crashed;
        NodeId link = 0;
        {
            Mounted ext(*device);
            link = Value(ext->SymbolicLink(ext->Root(), "magic-link", target));
            Succeeds(ext->Sync());
            crashed = CopyOf(*device);
        }
        const std::vector<std::uint8_t> replayed = BothReplays(*crashed, "transaction-escape-link");
        const RawVolume raw{const_cast<std::vector<std::uint8_t>&>(replayed)};
        const std::uint64_t block = raw.FileBlock(static_cast<std::uint32_t>(link), 0);
        const std::string dump = Logdump(*crashed);
        INFO(dump);
        bool found = false;
        for (const LoggedBlock& entry : LoggedBlocks(dump))
        {
            found = found || (entry.block == block && (entry.flags & Ext::JournalTagEscaped) != 0);
        }
        REQUIRE(found);
        const std::unique_ptr<MemoryDevice> after = DeviceFrom(replayed);
        MountedFilesystem again(Ext2Type(), *after, true);
        std::array<char, 200> buffer{};
        const std::size_t length =
            Value(again->ReadLink(Value(Walk(*again, "magic-link")), buffer));
        REQUIRE(std::string(buffer.data(), length) == target);
    }
}

TEST_CASE("freeing a logged block revokes it, so replays never put the old copy over its new use")
{
    struct Case
    {
        std::string_view image;
        bool extents;
    };
    for (const Case& which : {Case{"ext3", false}, Case{"ext4", true}})
    {
        INFO(which.image);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(which.image));
        const std::uint32_t block_size = BlockSizeOf(LoadExtImage(which.image));
        std::unique_ptr<MemoryDevice> after_remove;
        std::set<std::uint64_t> logged_tree;
        std::uint64_t size = 0;
        {
            Mounted ext(*device);
            const NodeId a = Value(ext->Create(ext->Root(), "a.bin", 0644));
            std::uint64_t length = 0;
            if (which.extents)
            {
                // Interleaved with another file, every block of each is an extent of its own: the
                // tree is two levels deep.
                const NodeId b = Value(ext->Create(ext->Root(), "b.bin", 0644));
                for (std::uint32_t index = 0; index < 420; ++index)
                {
                    const std::string text = PatternAt(index * block_size, block_size);
                    REQUIRE(Value(ext->Write(a, index * std::uint64_t{block_size},
                                             BytesOfText(text))) == block_size);
                    REQUIRE(Value(ext->Write(b, index * std::uint64_t{block_size},
                                             BytesOfText(text))) == block_size);
                }
                length = 420 * std::uint64_t{block_size};
            }
            else
            {
                // Past the direct blocks and one single indirect block: a double indirect block
                // and the single indirect block under it.
                const std::string text = PatternAt(0, 300 * block_size);
                REQUIRE(Value(ext->Write(a, 0, BytesOfText(text))) == text.size());
                length = text.size();
            }
            Succeeds(ext->Sync());
            const std::unique_ptr<MemoryDevice> first = CopyOf(*device);
            const std::vector<std::uint8_t> recovered = ReplayedBytes(*first);
            const RawVolume raw{const_cast<std::vector<std::uint8_t>&>(recovered)};
            const std::set<std::uint64_t> logged = LoggedSet(Logdump(*first));
            if (which.extents)
            {
                std::size_t deepest = 0;
                for (std::uint32_t index = 0; index < 420; ++index)
                {
                    const std::vector<std::uint64_t> path =
                        raw.ExtentPath(static_cast<std::uint32_t>(a), index);
                    deepest = std::max(deepest, path.size());
                    logged_tree.insert(path.begin(), path.end());
                }
                REQUIRE(deepest == 2);
            }
            else
            {
                const auto inode = static_cast<std::uint32_t>(a);
                const std::uint64_t single = raw.InodeBlock(inode, 12);
                const std::uint64_t dbl = raw.InodeBlock(inode, 13);
                logged_tree = {single, dbl,
                               raw.Load32(raw.BlockOffset(static_cast<std::uint32_t>(dbl)))};
            }
            REQUIRE(logged_tree.size() >= 3);
            for (const std::uint64_t block : logged_tree)
            {
                REQUIRE(logged.count(block) == 1);
            }

            Succeeds(ext->Remove(ext->Root(), "a.bin"));
            Succeeds(ext->Sync());
            after_remove = CopyOf(*device);
            REQUIRE(RevokedSet(Logdump(*after_remove)) == logged_tree);

            // A new file takes every free block, the revoked ones among them, and writes its
            // data in place.
            const NodeId c = Value(ext->Create(ext->Root(), "c.bin", 0644));
            size = FillVolume(*ext, c);
            Succeeds(ext->Sync());
            const std::unique_ptr<MemoryDevice> second = CopyOf(*device);
            const std::vector<std::uint8_t> reused = ReplayedBytes(*second);
            for (const std::uint64_t block : logged_tree)
            {
                REQUIRE(BlockInUse(reused, block));
            }
            static_cast<void>(length);
            const std::vector<std::uint8_t> replayed = BothReplays(*second, "transaction-revoke");
            const std::unique_ptr<MemoryDevice> mounted = DeviceFrom(replayed);
            MountedFilesystem again(Ext2Type(), *mounted, true);
            REQUIRE(ReadAll(*again, Value(Walk(*again, "c.bin"))) == PatternAt(0, size));
            Succeeds(ext.Unmount());
        }
        // The checkpoint at unmount writes what is still logged, but not the revoked blocks.
        CheckClean(*device, "transaction-revoke-unmounted");
        MountedFilesystem again(Ext2Type(), *device, true);
        REQUIRE(ReadAll(*again, Value(Walk(*again, "c.bin"))) == PatternAt(0, size));
    }
}

TEST_CASE("a block freed in a transaction is not allocated again before it commits")
{
    for (const std::string_view image : {"ext3", "ext4"})
    {
        INFO(image);
        const std::uint32_t block_size = BlockSizeOf(LoadExtImage(image));
        const std::string text = PatternAt(0, 20 * block_size);
        const auto blocks_of = [&](MemoryDevice& device, NodeId node)
        {
            const std::unique_ptr<MemoryDevice> copy = CopyOf(device);
            const std::vector<std::uint8_t> recovered = ReplayedBytes(*copy);
            const std::vector<std::uint64_t> list =
                BlocksOf(recovered, static_cast<std::uint32_t>(node), 20);
            return std::set<std::uint64_t>(list.begin(), list.end());
        };

        // With free space elsewhere the new file does not take the old one's blocks.
        {
            const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(image));
            Mounted ext(*device);
            const NodeId x = WriteNewFile(*ext, "x.bin", text);
            Succeeds(ext->Sync());
            const std::set<std::uint64_t> old_blocks = blocks_of(*device, x);
            Succeeds(ext->Remove(ext->Root(), "x.bin"));
            const NodeId y = WriteNewFile(*ext, "y.bin", text);
            REQUIRE(ActivityOf(*ext).retries == 0);
            Succeeds(ext->Sync());
            const std::set<std::uint64_t> new_blocks = blocks_of(*device, y);
            for (const std::uint64_t block : new_blocks)
            {
                REQUIRE(old_blocks.count(block) == 0);
            }
            Succeeds(ext.Unmount());
            CheckClean(*device, "transaction-noreuse");
        }

        // On a volume full but for the old file's blocks, the new file waits for the commit: the
        // write fails for lack of space, the transaction commits, and the one retry succeeds.
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(image));
        Mounted ext(*device);
        const NodeId x = WriteNewFile(*ext, "x.bin", text);
        const NodeId filler = Value(ext->Create(ext->Root(), "filler.bin", 0644));
        FillVolume(*ext, filler);
        Succeeds(ext->Sync());
        const std::set<std::uint64_t> old_blocks = blocks_of(*device, x);
        const std::uint32_t transactions = ActivityOf(*ext).transactions;
        Succeeds(ext->Remove(ext->Root(), "x.bin"));
        const NodeId y = Value(ext->Create(ext->Root(), "y.bin", 0644));
        REQUIRE(Value(ext->Write(y, 0, BytesOfText(text))) == text.size());
        const JournalActivity activity = ActivityOf(*ext);
        REQUIRE(activity.retries == 1);
        REQUIRE(activity.transactions == transactions + 1);
        Succeeds(ext->Sync());
        REQUIRE(blocks_of(*device, y) == old_blocks);
        REQUIRE(ReadAll(*ext, y) == text);
        Succeeds(ext.Unmount());
        CheckClean(*device, "transaction-retry");
    }
}

namespace
{

// One test case per image, so that CTest runs them side by side.
void WrapAndCheckpoint(std::string_view image)
{
    {
        INFO(image);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(image));
        std::map<std::string, std::string> model;
        std::vector<std::unique_ptr<MemoryDevice>> copies;
        {
            Mounted ext(*device);
            const NodeId root = ext->Root();
            std::map<std::string, NodeId> nodes;
            for (int step = 0; step < 600; ++step)
            {
                const std::string name = "f" + std::to_string(step % 30);
                std::string& contents = model[name];
                if (nodes.count(name) == 0)
                {
                    nodes[name] = Value(ext->Create(root, name, 0644));
                    contents.clear();
                }
                else
                {
                    const NodeId node = nodes[name];
                    switch ((step / 30) % 4)
                    {
                    case 0:
                    {
                        const std::string text = PatternAt(step, 150 + (step * 37) % 900);
                        REQUIRE(WriteAt(*ext, node, contents.size(), text) == text.size());
                        contents += text;
                        break;
                    }
                    case 1:
                        contents.resize(contents.size() / 2);
                        Succeeds(ext->Truncate(node, contents.size()));
                        break;
                    case 2:
                    {
                        AttributeChanges changes{};
                        changes.mode = step % 2 == 0 ? 0600 : 0644;
                        Succeeds(ext->SetAttributes(node, changes));
                        break;
                    }
                    default:
                        Succeeds(ext->Remove(root, name));
                        nodes.erase(name);
                        model.erase(name);
                        break;
                    }
                }
                Succeeds(ext->Sync());
                const JournalActivity activity = ActivityOf(*ext);
                const JournalSuperblock journal = JournalSuperblockOf(*device);
                // A log that goes round the end of the ring.
                if (copies.size() < 10 && step % 9 == 0 && journal.start != 0 &&
                    activity.head < journal.start)
                {
                    copies.push_back(CopyOf(*device));
                }
            }
            const JournalActivity activity = ActivityOf(*ext);
            REQUIRE(activity.wraps >= 2);
            REQUIRE(activity.checkpoints >= 2);
            REQUIRE(activity.transactions >= 400);
            Succeeds(ext.Unmount());
        }
        REQUIRE(copies.size() == 10);
        CheckClean(*device, "transaction-wrap");
        {
            MountedFilesystem again(Ext2Type(), *device, true);
            std::vector<std::string> names;
            for (const auto& [name, contents] : model)
            {
                names.push_back(name);
                REQUIRE(ReadAll(*again, Value(Walk(*again, name))) == contents);
            }
            REQUIRE(NamesIn(*again, "").size() >= names.size());
        }
        std::size_t index = 0;
        for (const std::unique_ptr<MemoryDevice>& copy : copies)
        {
            INFO("copy " << index);
            BothReplays(*copy, "transaction-wrapped-copy");
            ++index;
        }
    }
}

} // namespace

TEST_CASE("the log wraps and is checkpointed, and copies of it taken on the way replay: ext3")
{
    WrapAndCheckpoint("ext3");
}

TEST_CASE("the log wraps and is checkpointed, and copies of it taken on the way replay: ext4")
{
    WrapAndCheckpoint("ext4");
}

TEST_CASE("the log wraps and is checkpointed, and copies of it taken on the way replay: ext4-4k")
{
    WrapAndCheckpoint("ext4-4k");
}

TEST_CASE("a transaction commits when it grows, and each commit and checkpoint costs two flushes")
{
    // 2000 changes without a Sync: the transactions commit on their own, none past the limit.
    {
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage("ext3"));
        const JournalSuperblock journal = JournalSuperblockOf(*device);
        const std::uint32_t hard = journal.maximum_length - journal.first - 1;
        Mounted ext(*device);
        const NodeId directory = Value(ext->MakeDirectory(ext->Root(), "p", 0755));
        std::vector<NodeId> files;
        for (int index = 0; index < 1000; ++index)
        {
            files.push_back(Value(
                ext->Create(directory, "file-with-a-longer-name-" + std::to_string(index), 0644)));
        }
        for (const NodeId file : files)
        {
            AttributeChanges changes{};
            changes.mode = 0600;
            Succeeds(ext->SetAttributes(file, changes));
        }
        const JournalActivity activity = ActivityOf(*ext);
        REQUIRE(activity.transactions >= 2);
        REQUIRE(activity.largest_transaction <= hard);
        REQUIRE(activity.largest_transaction > 100);
        Succeeds(ext.Unmount());
        CheckClean(*device, "transaction-grow");
        MountedFilesystem again(Ext2Type(), *device, true);
        REQUIRE(NamesIn(*again, "p").size() == 1000);
    }

    // Nothing to commit costs nothing; a commit and a checkpoint cost two flushes each.
    for (const std::string_view image : {"ext3", "ext4"})
    {
        INFO(image);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(image));
        Mounted ext(*device);
        const std::uint64_t writes = device->write_count;
        Succeeds(ext->Sync());
        REQUIRE(device->write_count == writes);
        REQUIRE(ActivityOf(*ext).transactions == 0);
        WriteNewFile(*ext, "flush.txt", "x");
        std::uint32_t flushes = device->flush_count;
        Succeeds(ext->Sync());
        REQUIRE(device->flush_count - flushes == 2);
        Succeeds(ext->Sync());
        REQUIRE(device->write_count > writes);
        REQUIRE(ActivityOf(*ext).transactions == 1);
        flushes = device->flush_count;
        Succeeds(ext.Unmount());
        // Sync's, the checkpoint's two, and the one after clearing needs_recovery.
        REQUIRE(device->flush_count - flushes == 4);
        CheckClean(*device, "transaction-flush");
    }

    // A committed layer over its limit is written to its places, with the ring far from full.
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage("ext3"));
    Mounted ext(*device);
    Core(*ext).LimitJournal({.soft = 8, .hard = 24, .committed = 32});
    std::uint32_t first_checkpoint = 0;
    for (int index = 0; index < 60; ++index)
    {
        Value(ext->MakeDirectory(ext->Root(), "d" + std::to_string(index), 0755));
        Succeeds(ext->Sync());
        const JournalActivity activity = ActivityOf(*ext);
        if (activity.checkpoints != 0 && first_checkpoint == 0)
        {
            first_checkpoint = activity.transactions;
        }
        REQUIRE(activity.largest_transaction <= 24);
    }
    REQUIRE(first_checkpoint != 0);
    REQUIRE(first_checkpoint < 40);
    Succeeds(ext.Unmount());
    CheckClean(*device, "transaction-memory");
}

TEST_CASE("a change that overflows the transaction commits what runs and retries, or fails whole")
{
    // Several changes that do not fit together run one after the other.
    {
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage("ext3"));
        Mounted ext(*device);
        Core(*ext).LimitJournal({.soft = Unlimited, .hard = 10, .committed = 32});
        for (int index = 0; index < 40; ++index)
        {
            Value(ext->MakeDirectory(ext->Root(), "r" + std::to_string(index), 0755));
        }
        REQUIRE(ActivityOf(*ext).retries >= 1);
        REQUIRE(ActivityOf(*ext).largest_transaction <= 10);
        for (int index = 0; index < 40; ++index)
        {
            Value(Walk(*ext, "r" + std::to_string(index)));
        }
        Succeeds(ext.Unmount());
        CheckClean(*device, "transaction-retries");
    }

    // One that does not fit even alone fails with NoSpace and changes nothing.
    struct Case
    {
        std::string_view image;
        std::uint32_t hard;
        const char* name;
    };
    // Freed blocks are dropped, not written, so a transaction that frees a file's blocks holds few:
    // one block (the superblock) is what even the first piece of a split truncation or removal
    // does not fit in, a name in a hashed directory with checksums needs its index
    // blocks converted, and a write maps blocks.
    for (const Case& which :
         {Case{"ext3", 3, "a 2 MiB write"}, Case{"ext3", 1, "a truncation"},
          Case{"ext3", 1, "a removal"}, Case{"ext4-extents", 7, "a name in a hashed directory"}})
    {
        INFO(which.image << ": " << which.name);
        std::vector<std::uint8_t> original = LoadExtImage(which.image);
        const std::string name = which.name;
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(original);
        const std::string big = PatternAt(0, 3 * 1024 * 1024);
        if (which.image == "ext3")
        {
            Mounted ext(*device);
            WriteNewFile(*ext, "t.bin", big);
            Value(ext->Create(ext->Root(), "w.bin", 0644));
        }
        Mounted ext(*device);
        Core(*ext).LimitJournal({.soft = Unlimited, .hard = which.hard, .committed = Unlimited});
        const NodeId root = ext->Root();
        NodeId subject = 0;
        std::vector<std::string> names =
            NamesIn(*ext, name == "a name in a hashed directory" ? "large" : "");
        if (name == "a 2 MiB write")
        {
            subject = Value(Walk(*ext, "w.bin"));
        }
        else if (name != "a name in a hashed directory")
        {
            subject = Value(Walk(*ext, "t.bin"));
        }
        const VolumeStatistics before = Value(ext->Statistics());
        const std::uint32_t transactions = ActivityOf(*ext).transactions;
        if (name == "a 2 MiB write")
        {
            REQUIRE(ErrorOf(ext->Write(subject, 0, BytesOfText(PatternAt(0, 2 * 1024 * 1024)))) ==
                    Error::NoSpace);
            REQUIRE(Value(ext->GetAttributes(subject)).size == 0);
        }
        else if (name == "a truncation")
        {
            REQUIRE(ext->Truncate(subject, 0).error() == Error::NoSpace);
            REQUIRE(ReadAll(*ext, subject) == big);
        }
        else if (name == "a removal")
        {
            REQUIRE(ext->Remove(root, "t.bin").error() == Error::NoSpace);
            REQUIRE(ReadAll(*ext, Value(Walk(*ext, "t.bin"))) == big);
        }
        else
        {
            const NodeId large = Value(Walk(*ext, "large"));
            REQUIRE(ErrorOf(ext->Create(large, "a-name-that-needs-the-index-dropped", 0644)) ==
                    Error::NoSpace);
            REQUIRE(ErrorOf(ext->Lookup(large, "a-name-that-needs-the-index-dropped")) ==
                    Error::NotFound);
        }
        const VolumeStatistics after = Value(ext->Statistics());
        REQUIRE(after.free_blocks == before.free_blocks);
        REQUIRE(after.free_nodes == before.free_nodes);
        REQUIRE(NamesIn(*ext, name == "a name in a hashed directory" ? "large" : "") == names);
        // Nothing of it is left to commit.
        const std::uint64_t writes = device->write_count;
        Succeeds(ext->Sync());
        REQUIRE(device->write_count == writes);
        REQUIRE(ActivityOf(*ext).transactions == transactions);
        Succeeds(ext.Unmount());
        CheckClean(*device, "transaction-alone");
    }
}

TEST_CASE("a step that fails leaves nothing behind, for want of space or of a read")
{
    // A full directory on a full volume: the new inode is taken before the directory needs a
    // block, and the failure gives it back.
    const auto prepare = [](MemoryDevice& device, Mounted& ext)
    {
        const NodeId directory = Value(ext->MakeDirectory(ext->Root(), "full", 0755));
        for (int index = 0; index < 3; ++index)
        {
            Value(ext->Create(directory, NumberedName("n", index, 255), 0644));
        }
        FillVolume(*ext, Value(ext->Create(ext->Root(), "filler", 0644)));
        Succeeds(ext->Sync());
        static_cast<void>(device);
        return directory;
    };
    std::set<std::uint64_t> control;
    {
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage("ext3"));
        Mounted ext(*device);
        prepare(*device, ext);
        AttributeChanges changes{};
        changes.mode = 0600;
        Succeeds(ext->SetAttributes(Value(Walk(*ext, "hello.txt")), changes));
        Succeeds(ext->Sync());
        control = LoggedSet(Logdump(*CopyOf(*device)));
    }
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage("ext3"));
    Mounted ext(*device);
    const NodeId directory = prepare(*device, ext);
    const VolumeStatistics before = Value(ext->Statistics());
    const std::vector<std::string> names = NamesIn(*ext, "full");
    AttributeChanges changes{};
    changes.mode = 0600;
    Succeeds(ext->SetAttributes(Value(Walk(*ext, "hello.txt")), changes));
    const std::string refused = NumberedName("n", 3, 255);
    REQUIRE(ErrorOf(ext->Create(directory, refused, 0644)) == Error::NoSpace);
    REQUIRE(ErrorOf(ext->MakeDirectory(directory, refused, 0755)) == Error::NoSpace);
    const VolumeStatistics after = Value(ext->Statistics());
    REQUIRE(after.free_nodes == before.free_nodes);
    REQUIRE(after.free_blocks == before.free_blocks);
    REQUIRE(NamesIn(*ext, "full") == names);
    REQUIRE(ErrorOf(ext->Lookup(directory, refused)) == Error::NotFound);
    Succeeds(ext->Sync());
    REQUIRE(LoggedSet(Logdump(*CopyOf(*device))) == control);
    Succeeds(ext.Unmount());
    CheckClean(*device, "transaction-nospace");
}

TEST_CASE("a read that fails after the inode was taken gives the inode back")
{
    for (const std::string_view image : {"ext4", "ext3", "ext4-4k"})
    {
        INFO(image);
        std::vector<std::uint8_t> original = LoadExtImage(image);
        const RawVolume raw{original};
        // The first free inode of group 0, and the block of its inode table that it lies in.
        const std::uint32_t bitmap = static_cast<std::uint32_t>(GroupOf(original, 0).inode_bitmap);
        std::uint32_t inode = raw.Load32(SuperblockStart + 84);
        while ((original[raw.BlockOffset(bitmap) + (inode - 1) / 8] >> ((inode - 1) % 8) & 1U) != 0)
        {
            ++inode;
        }
        const std::uint64_t table_block = InodeTableBlock(original, inode);
        REQUIRE(table_block != InodeTableBlock(original, 2));
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(original);
        Mounted ext(*device);
        const VolumeStatistics before = Value(ext->Statistics());
        const std::vector<std::string> names = NamesIn(*ext, "");
        const std::uint32_t transactions = ActivityOf(*ext).transactions;
        device->FailReadsAt(table_block * raw.BlockSize() / 512);
        REQUIRE(ErrorOf(ext->Create(ext->Root(), "io.txt", 0644)) == Error::Io);
        REQUIRE(Value(ext->Statistics()).free_nodes == before.free_nodes);
        REQUIRE(NamesIn(*ext, "") == names);
        device->RestoreReads();
        REQUIRE(Value(ext->Create(ext->Root(), "io.txt", 0644)) == inode);
        REQUIRE(Value(ext->Statistics()).free_nodes == before.free_nodes - 1);
        Succeeds(ext->Sync());
        REQUIRE(ActivityOf(*ext).transactions == transactions + 1);
        Succeeds(ext.Unmount());
        CheckClean(*device, "transaction-io");
    }
}

TEST_CASE("damage found in a step is committed with the earlier changes and ends writing")
{
    std::vector<std::uint8_t> bytes = LoadExtImage("ext4");
    {
        const RawVolume raw{bytes};
        const GroupDescriptor group = GroupOf(bytes, 0);
        bytes[raw.BlockOffset(static_cast<std::uint32_t>(group.block_bitmap)) + 10] ^= 0x55;
    }
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
    {
        Mounted ext(*device);
        AttributeChanges changes{};
        changes.mode = 0600;
        Succeeds(ext->SetAttributes(Value(Walk(*ext, "hello.txt")), changes));
        const NodeId file = Value(ext->Create(ext->Root(), "damaged.txt", 0644));
        REQUIRE(ErrorOf(ext->Write(file, 0, BytesOfText(Pattern(3000)))) == Error::Corrupt);
        REQUIRE(ErrorOf(ext->Create(ext->Root(), "later.txt", 0644)) == Error::ReadOnly);
        Succeeds(ext.Unmount());
    }
    std::vector<std::uint8_t> after = BytesOf(*device);
    RawVolume raw{after};
    REQUIRE((raw.Load16(SuperblockStart + 58) & ErrorsState) != 0);
    REQUIRE((raw.Load32(SuperblockStart + 96) & NeedsRecovery) == 0);
    MountedFilesystem again(Ext2Type(), *device, false);
    REQUIRE_FALSE(again->GetCapabilities().writable);
    REQUIRE(Value(again->GetAttributes(Value(Walk(*again, "hello.txt")))).mode == 0600);
    REQUIRE(Value(again->GetAttributes(Value(Walk(*again, "damaged.txt")))).size == 0);
    REQUIRE(ErrorOf(Walk(*again, "later.txt")) == Error::NotFound);
}

TEST_CASE(
    "a device that fails inside a commit ends the journal's writes and the next mount recovers")
{
    for (std::uint64_t cut = 0; cut < 14; ++cut)
    {
        INFO("cut after " << cut << " writes");
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage("ext4"));
        {
            Mounted ext(*device);
            WriteNewFile(*ext, "first.txt", "committed");
            Succeeds(ext->Sync());
            WriteNewFile(*ext, "second.txt", "maybe");
            device->FailWritesAfter(device->write_count + cut);
            const Status synced = ext->Sync();
            // A commit takes about ten writes; a later cut leaves it whole.
            REQUIRE(synced.has_value() == (cut >= 9));
            if (synced)
            {
                REQUIRE_FALSE(ActivityOf(*ext).aborted);
                device->FailWritesAfter(UINT64_MAX);
                Succeeds(ext.Unmount());
            }
            else
            {
                REQUIRE(synced.error() == Error::Io);
                REQUIRE(ActivityOf(*ext).aborted);
                // The device works again, but nothing more is written.
                device->FailWritesAfter(UINT64_MAX);
                const std::uint64_t writes = device->write_count;
                const std::uint32_t flushes = device->flush_count;
                REQUIRE(ErrorOf(ext->Create(ext->Root(), "third.txt", 0644)) == Error::ReadOnly);
                Succeeds(ext->Sync());
                Succeeds(ext.Unmount());
                REQUIRE(device->write_count == writes);
                REQUIRE(device->flush_count == flushes);
            }
        }
        {
            Mounted ext(*device);
            REQUIRE(ext->GetCapabilities().writable);
            REQUIRE(ReadAll(*ext, Value(Walk(*ext, "first.txt"))) == "committed");
            Succeeds(ext.Unmount());
        }
        CheckClean(*device, "transaction-abort");
    }
}

TEST_CASE("a writable mount keeps the features a journal already has and checksums every block")
{
    struct Case
    {
        std::string_view image;
        std::uint32_t compatible;
        std::uint32_t incompatible;
        std::uint32_t expected;
    };
    for (const Case& which : {Case{"ext3", Ext::JournalCompatibleChecksum, 0, Revoke},
                              Case{"ext4", 0, Async | V3, Async | V3 | Revoke | Wide},
                              Case{"ext4", 0, Revoke | V2, Revoke | V2 | Wide},
                              Case{"ext4-64bit", 0, Revoke | Wide | V2, Revoke | Wide | V2},
                              Case{"ext4-journal-gdtcsum", 0, 0, Revoke | Wide}})
    {
        INFO(which.image << " with features " << which.incompatible);
        std::vector<std::uint8_t> bytes = LoadExtImage(which.image);
        SetFeatures(bytes, which.compatible, which.incompatible);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
        std::unique_ptr<MemoryDevice> crashed;
        {
            Mounted ext(*device);
            const JournalSuperblock journal = JournalSuperblockOf(*device);
            REQUIRE(journal.incompatible == which.expected);
            REQUIRE((journal.compatible & Ext::JournalCompatibleChecksum) == 0);
            for (int index = 0; index < 3; ++index)
            {
                WriteNewFile(*ext, "kept-" + std::to_string(index), PatternAt(index, 2000));
                Succeeds(ext->Sync());
            }
            crashed = CopyOf(*device);
            const std::unique_ptr<MemoryDevice> theirs = CopyOf(*crashed);
            // e2fsprogs reads and verifies what this core wrote with those features.
            const std::string dump = Logdump(*crashed);
            REQUIRE(CountOf(dump, "type 2 (commit block)") == 3);
            REQUIRE(CountOf(dump, "type 1 (descriptor block)") == 3);
        }
        BothReplays(*crashed, "transaction-features");
    }
}

#endif
