// SPDX-License-Identifier: MIT OR Apache-2.0
// Orphans on writable ext3 and ext4 volumes: the orphan file's format, what a mount makes of the
// orphans Linux leaves, which damaged ones it refuses, and changes too big for one transaction
// that are split in several with an orphan entry between the first and the last.
#include <catch2/catch_test_macros.hpp>

#include "ext/checksum.h"
#include "ext/ext2_filesystem.h"
#include "ext/superblock.h"
#include "fs/ext2.h"
#include "tests/ext2_image.h"
#include "tests/filesystem_helpers.h"
#include "tests/memory_device.h"
#include "tests/page_cache_support.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <span>
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
using Ext::JournalLimits;

constexpr std::uint64_t SuperblockStart = 1024;
constexpr std::uint32_t OrphanPresent = 0x10000;
constexpr std::uint32_t MetadataChecksum = 0x400;

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

class Mounted
{
public:
    explicit Mounted(MemoryDevice& volume, bool read_only = false)
    {
        filesystem = Value(Ext2Type().Mount(Mountable(volume), {.read_only = read_only}));
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

std::string PatternAt(std::uint64_t offset, std::size_t length, unsigned shift = 0)
{
    std::string text(length, '\0');
    for (std::size_t index = 0; index < length; ++index)
    {
        text[index] = static_cast<char>('a' + (offset + index + shift) % 23);
    }
    return text;
}

Ext::Superblock SuperblockOf(const std::vector<std::uint8_t>& bytes)
{
    return Ext::DecodeSuperblock(std::span<const std::uint8_t>(bytes)
                                     .subspan(SuperblockStart, Ext::SuperblockSize)
                                     .first<Ext::SuperblockSize>());
}

// What the volume's orphan structures hold, read from its bytes: the head of the list, whether
// orphan_present is set, and the inodes in the orphan file's slots.
struct OrphanState
{
    std::uint32_t last_orphan = 0;
    bool present = false;
    std::vector<std::uint32_t> slots;
};

OrphanState StateOf(MemoryDevice& device, std::string_view name)
{
    const std::vector<std::uint8_t> bytes = BytesOf(device);
    const Ext::Superblock superblock = SuperblockOf(bytes);
    OrphanState state;
    state.last_orphan = superblock.last_orphan;
    state.present = (superblock.read_only_compatible & OrphanPresent) != 0;
    if ((superblock.compatible & Ext::CompatibleOrphanFile) != 0 && HaveE2fsck())
    {
        const ToolRun cat =
            RunDebugfs(device, name, "cat <" + std::to_string(superblock.orphan_file_inode) + ">");
        REQUIRE(cat.ran);
        REQUIRE(cat.output.size() % superblock.block_size == 0);
        const std::size_t slots = Ext::OrphansPerBlock(superblock.block_size);
        for (std::size_t block = 0; block < cat.output.size() / superblock.block_size; ++block)
        {
            const auto* raw = reinterpret_cast<const std::uint8_t*>(cat.output.data()) +
                              block * superblock.block_size;
            for (std::size_t slot = 0; slot < slots; ++slot)
            {
                const std::uint32_t node = raw[slot * 4] | raw[slot * 4 + 1] << 8 |
                                           raw[slot * 4 + 2] << 16 |
                                           static_cast<std::uint32_t>(raw[slot * 4 + 3]) << 24;
                if (node != 0)
                {
                    state.slots.push_back(node);
                }
            }
        }
    }
    return state;
}

void RequireNoOrphans(MemoryDevice& device, std::string_view name)
{
    const OrphanState state = StateOf(device, name);
    REQUIRE(state.last_orphan == 0);
    REQUIRE_FALSE(state.present);
    REQUIRE(state.slots.empty());
}

// Names, types, sizes, links and contents of the whole tree, as one text.
void SnapshotInto(Filesystem& filesystem, NodeId directory, const std::string& path,
                  std::string& out)
{
    for (const std::string& listed : Sorted(List(filesystem, directory)))
    {
        std::string name = listed;
        const bool is_directory = name.ends_with('/');
        if (is_directory)
        {
            name.pop_back();
        }
        if (name == "." || name == "..")
        {
            continue;
        }
        const NodeId node = Value(filesystem.Lookup(directory, name));
        const Attributes attributes = Value(filesystem.GetAttributes(node));
        // A directory's size is how many blocks its names took, which e2fsck may repack.
        out += path + name + " type " + std::to_string(static_cast<int>(attributes.type)) +
               " size " + std::to_string(is_directory ? 0 : attributes.size) + " links " +
               std::to_string(attributes.link_count);
        if (attributes.type == NodeType::File)
        {
            std::uint64_t hash = 1469598103934665603ULL;
            for (const char byte : ReadAll(filesystem, node))
            {
                hash = (hash ^ static_cast<std::uint8_t>(byte)) * 1099511628211ULL;
            }
            out += " hash " + std::to_string(hash);
        }
        out += '\n';
        if (is_directory)
        {
            SnapshotInto(filesystem, node, path + name + "/", out);
        }
    }
}

std::string SnapshotOf(MemoryDevice& device)
{
    Mounted ext(device, true);
    std::string text;
    SnapshotInto(*ext, ext->Root(), "/", text);
    return text;
}

// The first line the two texts do not share, from each, or nothing when they are the same.
std::string FirstDifference(const std::string& ours, const std::string& theirs)
{
    std::size_t at = 0;
    while (at < ours.size() && at < theirs.size() && ours[at] == theirs[at])
    {
        ++at;
    }
    if (at == ours.size() && at == theirs.size())
    {
        return {};
    }
    const auto line = [&](const std::string& text)
    {
        const std::size_t start = text.rfind('\n', at == 0 ? 0 : at - 1);
        const std::size_t begin = start == std::string::npos ? 0 : start + 1;
        return text.substr(begin, text.find('\n', at) - begin);
    };
    return "ours: " + line(ours) + " / theirs: " + line(theirs);
}

// e2fsck -fy rehashes the directories it finds linear, whatever else it does, which moves blocks;
// both volumes go through it before their free blocks are compared.
std::string FreeRangesAfterFix(MemoryDevice& device, std::string_view name)
{
    const std::unique_ptr<MemoryDevice> copy = CopyOf(device);
    const ToolRun fixed = FixWithE2fsck(*copy, name);
    REQUIRE(fixed.status <= 1);
    return FreeRangesOf(*copy, name);
}

std::uint32_t InodeOf(Filesystem& filesystem, std::string_view path)
{
    return static_cast<std::uint32_t>(Value(Walk(filesystem, path)));
}

// Fills the orphan file's first block with these inodes, with the checksum a writer keeps, and
// sets orphan_present.
void PutInOrphanFile(std::vector<std::uint8_t>& bytes, const std::vector<std::uint32_t>& nodes)
{
    RawVolume raw{bytes};
    const Ext::Superblock superblock = SuperblockOf(bytes);
    const std::uint32_t inode = superblock.orphan_file_inode;
    const std::uint64_t block = raw.FileBlock(inode, 0);
    REQUIRE(block != 0);
    std::uint8_t* data = bytes.data() + raw.BlockOffset(static_cast<std::uint32_t>(block));
    for (std::size_t index = 0; index < nodes.size(); ++index)
    {
        for (std::size_t byte = 0; byte < 4; ++byte)
        {
            data[index * 4 + byte] = static_cast<std::uint8_t>(nodes[index] >> (8 * byte));
        }
    }
    if ((superblock.read_only_compatible & MetadataChecksum) != 0)
    {
        const std::uint32_t sum = Ext::OrphanBlockChecksum(
            Ext::VolumeChecksumSeed(superblock), inode, raw.Load32(raw.InodeOffset(inode) + 100),
            block,
            std::span<const std::uint8_t>(data, superblock.block_size - Ext::OrphanTailSize));
        for (std::size_t byte = 0; byte < 4; ++byte)
        {
            data[superblock.block_size - 4 + byte] = static_cast<std::uint8_t>(sum >> (8 * byte));
        }
    }
    raw.Store32(SuperblockStart + 100, raw.Load32(SuperblockStart + 100) | OrphanPresent);
    if ((superblock.read_only_compatible & MetadataChecksum) != 0)
    {
        raw.UpdateSuperblockChecksum();
    }
}

std::uint64_t OrphanBlockOffset(std::vector<std::uint8_t>& bytes)
{
    RawVolume raw{bytes};
    const std::uint64_t block = raw.FileBlock(SuperblockOf(bytes).orphan_file_inode, 0);
    REQUIRE(block != 0);
    return raw.BlockOffset(static_cast<std::uint32_t>(block));
}

// The volume with three files: a.bin and c.bin unlinked and b.bin cut to 1000 bytes, all three
// left as orphans the way a crash in the middle of a removal or truncation leaves them: on the
// s_last_orphan list (c, b, a), or in the orphan file's slots.
struct OrphanVolume
{
    std::vector<std::uint8_t> bytes;
    std::uint32_t a, b, c;
};

OrphanVolume MakeOrphanVolume(std::string_view image, bool in_file)
{
    std::vector<std::uint8_t> bytes = LoadExtImage(image);
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
    OrphanVolume volume{};
    {
        Mounted ext(*device);
        WriteNewFile(*ext, "a.bin", PatternAt(0, 40000));
        WriteNewFile(*ext, "b.bin", PatternAt(0, 70000, 5));
        WriteNewFile(*ext, "c.bin", PatternAt(0, 30000, 9));
        volume.a = InodeOf(*ext, "a.bin");
        volume.b = InodeOf(*ext, "b.bin");
        volume.c = InodeOf(*ext, "c.bin");
        Succeeds(ext.Unmount());
    }
    std::string script = "unlink a.bin\nunlink c.bin\n";
    script += "sif <" + std::to_string(volume.a) + "> links_count 0\n";
    script += "sif <" + std::to_string(volume.c) + "> links_count 0\n";
    script += "sif <" + std::to_string(volume.b) + "> size 1000\n";
    if (!in_file)
    {
        script += "sif <" + std::to_string(volume.c) + "> dtime " + std::to_string(volume.b) + "\n";
        script += "sif <" + std::to_string(volume.b) + "> dtime " + std::to_string(volume.a) + "\n";
        script += "ssv last_orphan " + std::to_string(volume.c) + "\n";
    }
    REQUIRE(ScriptWithDebugfs(*device, "orphan-make", script, {}).status == 0);
    volume.bytes = BytesOf(*device);
    if (in_file)
    {
        PutInOrphanFile(volume.bytes, {volume.a, volume.b, volume.c});
    }
    return volume;
}

void Prepare(MemoryDevice& device, std::size_t big)
{
    Mounted ext(device);
    if (big != 0)
    {
        WriteNewFile(*ext, "big.bin", PatternAt(0, big));
    }
    WriteNewFile(*ext, "src.txt", "src");
    WriteNewFile(*ext, "w.bin", "");
    // Three MiB that are all hole.
    const NodeId sparse = Value(ext->Create(ext->Root(), "hole.bin", 0644));
    Succeeds(ext->Truncate(sparse, 3 << 20));
    Succeeds(ext.Unmount());
}

} // namespace

TEST_CASE("the orphan file's blocks carry the checksum mke2fs writes, and the fields decode")
{
    for (const std::string_view image :
         {"ext4", "ext4-blockmap", "ext4-64bit", "ext4-4k", "ext4-journal-gdtcsum"})
    {
        INFO(image);
        std::vector<std::uint8_t> bytes = LoadExtImage(image);
        RawVolume raw{bytes};
        const Ext::Superblock superblock = SuperblockOf(bytes);
        REQUIRE((superblock.compatible & Ext::CompatibleOrphanFile) != 0);
        REQUIRE(superblock.orphan_file_inode != 0);
        const std::uint32_t inode = superblock.orphan_file_inode;
        const std::uint32_t generation = raw.Load32(raw.InodeOffset(inode) + 100);
        const bool checksums = (superblock.read_only_compatible & MetadataChecksum) != 0;
        for (std::uint32_t logical = 0; logical < 12; ++logical)
        {
            const std::uint64_t block = raw.FileBlock(inode, logical);
            REQUIRE(block != 0);
            const std::uint8_t* data =
                bytes.data() + raw.BlockOffset(static_cast<std::uint32_t>(block));
            const std::uint32_t size = superblock.block_size;
            const std::span<const std::uint8_t> slots(data, size - Ext::OrphanTailSize);
            REQUIRE(raw.Load32(raw.BlockOffset(static_cast<std::uint32_t>(block)) + size - 8) ==
                    Ext::OrphanBlockMagic);
            const std::uint32_t stored =
                raw.Load32(raw.BlockOffset(static_cast<std::uint32_t>(block)) + size - 4);
            REQUIRE(Ext::OrphansPerBlock(size) * 4 == slots.size());
            if (checksums)
            {
                REQUIRE(Ext::OrphanBlockChecksum(Ext::VolumeChecksumSeed(superblock), inode,
                                                 generation, block, slots) == stored);
            }
            else
            {
                REQUIRE(stored == 0);
            }
        }
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
        const ToolRun stats = RunDebugfs(*device, "orphan-stats", "stats");
        if (stats.ran)
        {
            REQUIRE(stats.output.find("Orphan file inode:        " + std::to_string(inode)) !=
                    std::string::npos);
        }
    }

    // s_last_orphan is where dumpe2fs and debugfs say it is.
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage("ext3"));
    const ToolRun changed = ChangeWithDebugfs(*device, "orphan-ssv", "ssv last_orphan 77");
    if (changed.ran)
    {
        REQUIRE(changed.status == 0);
        REQUIRE(SuperblockOf(BytesOf(*device)).last_orphan == 77);
        REQUIRE(RunDebugfs(*device, "orphan-ssv", "stats").output.find("First orphan inode:") !=
                std::string::npos);
    }
}

TEST_CASE("orphans Linux left are finished by a writable mount, as e2fsck finishes them")
{
    struct Case
    {
        std::string_view image;
        bool in_file;
    };
    for (const Case& which : {Case{"ext3", false}, Case{"ext4", true}, Case{"ext4-blockmap", true},
                              Case{"ext3-groups", false}, Case{"ext4-extents", true}})
    {
        INFO(which.image << (which.in_file ? ": orphan file" : ": list"));
        const OrphanVolume volume = MakeOrphanVolume(which.image, which.in_file);
        const std::unique_ptr<MemoryDevice> crashed = DeviceFrom(volume.bytes);

        // A read-only mount reads them as they are and writes nothing.
        {
            const std::unique_ptr<MemoryDevice> copy = CopyOf(*crashed);
            Mounted ext(*copy, true);
            REQUIRE(ErrorOf(ext->Lookup(ext->Root(), "a.bin")) == Error::NotFound);
            REQUIRE(Value(ext->GetAttributes(Value(Walk(*ext, "b.bin")))).size == 1000);
            Succeeds(ext.Unmount());
            REQUIRE(copy->write_count == 0);
        }

        const std::unique_ptr<MemoryDevice> ours = CopyOf(*crashed);
        {
            Mounted ext(*ours);
            REQUIRE(ext->GetCapabilities().writable);
            REQUIRE(ActivityOf(*ext).orphans_processed == 3);
            REQUIRE(ErrorOf(ext->Lookup(ext->Root(), "a.bin")) == Error::NotFound);
            REQUIRE(ErrorOf(ext->Lookup(ext->Root(), "c.bin")) == Error::NotFound);
            const NodeId b = Value(Walk(*ext, "b.bin"));
            REQUIRE(Value(ext->GetAttributes(b)).size == 1000);
            REQUIRE(ReadAll(*ext, b) == PatternAt(0, 1000, 5));
            Succeeds(ext.Unmount());
        }
        RequireNoOrphans(*ours, "orphan-ours");
        CheckClean(*ours, "orphan-ours");

        const std::unique_ptr<MemoryDevice> theirs = CopyOf(*crashed);
        const ToolRun fixed = FixWithE2fsck(*theirs, "orphan-theirs");
        if (fixed.ran)
        {
            INFO(fixed.output);
            REQUIRE(fixed.status <= 1);
            CheckClean(*theirs, "orphan-theirs");
            const std::string tree = FirstDifference(SnapshotOf(*ours), SnapshotOf(*theirs));
            INFO(tree);
            REQUIRE(tree.empty());
            const std::string free =
                FirstDifference(FreeRangesAfterFix(*ours, "orphan-free-ours"),
                                FreeRangesAfterFix(*theirs, "orphan-free-theirs"));
            INFO(free);
            REQUIRE(free.empty());
        }
    }
}

TEST_CASE("orphans that cannot be trusted keep the volume read-only and unchanged")
{
    struct Case
    {
        std::string_view name;
        std::string_view image;
        std::function<void(std::vector<std::uint8_t>&)> change;
    };
    const auto debugfs_change = [](std::vector<std::uint8_t>& bytes, const std::string& script)
    {
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
        const ToolRun run = ScriptWithDebugfs(*device, "orphan-refuse", script, {});
        REQUIRE(run.ran);
        REQUIRE(run.status == 0);
        bytes = BytesOf(*device);
    };
    const std::vector<Case> cases = {
        {"a loop in the list", "ext3", [&](std::vector<std::uint8_t>& bytes)
         { debugfs_change(bytes, "sif <12> dtime 13\nsif <13> dtime 12\nssv last_orphan 12\n"); }},
        {"an entry past the inodes", "ext3", [&](std::vector<std::uint8_t>& bytes)
         { debugfs_change(bytes, "ssv last_orphan 99999\n"); }},
        {"an entry below the first ordinary inode", "ext3",
         [&](std::vector<std::uint8_t>& bytes) { debugfs_change(bytes, "ssv last_orphan 5\n"); }},
        {"a free inode in the list", "ext3",
         [&](std::vector<std::uint8_t>& bytes)
         {
             debugfs_change(bytes, "ssv last_orphan " +
                                       std::to_string(RawVolume{bytes}.InodeCount()) + "\n");
         }},
        {"a bad orphan file magic", "ext4", [](std::vector<std::uint8_t>& bytes)
         { bytes[OrphanBlockOffset(bytes) + SuperblockOf(bytes).block_size - 8] ^= 0xFF; }},
        {"a bad orphan file checksum", "ext4", [](std::vector<std::uint8_t>& bytes)
         { bytes[OrphanBlockOffset(bytes) + SuperblockOf(bytes).block_size - 4] ^= 0x01; }},
        {"a bad checksum in a later block", "ext4-4k",
         [](std::vector<std::uint8_t>& bytes)
         {
             RawVolume raw{bytes};
             const std::uint64_t block = raw.FileBlock(SuperblockOf(bytes).orphan_file_inode, 1);
             REQUIRE(block != 0);
             bytes[raw.BlockOffset(static_cast<std::uint32_t>(block)) +
                   SuperblockOf(bytes).block_size - 4] ^= 0x01;
         }},
        {"an inode in a slot that is free", "ext4", [](std::vector<std::uint8_t>& bytes)
         { PutInOrphanFile(bytes, {RawVolume{bytes}.InodeCount()}); }},
        {"an orphan file inode that is a directory", "ext4",
         [](std::vector<std::uint8_t>& bytes)
         {
             RawVolume raw{bytes};
             raw.Store32(SuperblockStart + 0x280, 2);
             raw.UpdateSuperblockChecksum();
         }},
        {"an orphan file inode that is not there", "ext4-blockmap",
         [](std::vector<std::uint8_t>& bytes)
         {
             RawVolume raw{bytes};
             raw.Store32(SuperblockStart + 0x280, 0);
             raw.UpdateSuperblockChecksum();
         }},
        {"orphans on a volume without a journal", "1k",
         [&](std::vector<std::uint8_t>& bytes) { debugfs_change(bytes, "ssv last_orphan 12\n"); }},
    };
    for (const Case& which : cases)
    {
        INFO(which.name << " on " << which.image);
        std::vector<std::uint8_t> bytes = LoadExtImage(which.image);
        which.change(bytes);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
        {
            Mounted ext(*device);
            REQUIRE_FALSE(ext->GetCapabilities().writable);
            REQUIRE(ErrorOf(ext->Create(ext->Root(), "x", 0644)) == Error::ReadOnly);
            Succeeds(ext.Unmount());
        }
        REQUIRE(device->write_count == 0);
        REQUIRE(device->flush_count == 0);
        RequireSameBytes(BytesOf(*device), bytes);
    }

    // orphan_present with nothing in the orphan file is cleared by a writable mount.
    std::vector<std::uint8_t> bytes = LoadExtImage("ext4");
    RawVolume raw{bytes};
    raw.Store32(SuperblockStart + 100, raw.Load32(SuperblockStart + 100) | OrphanPresent);
    raw.UpdateSuperblockChecksum();
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
    {
        Mounted ext(*device);
        REQUIRE(ext->GetCapabilities().writable);
        Succeeds(ext.Unmount());
    }
    RequireNoOrphans(*device, "orphan-present-only");
    CheckClean(*device, "orphan-present-only");
}

namespace
{

struct Limits
{
    std::string_view image;
    std::uint32_t hard;
    std::size_t big;
};

constexpr Limits SplitVolumes[] = {
    {"ext4-blockmap", 6, 5 << 20},
    {"ext3-groups", 5, 5 << 20},
    {"ext4-extents", 6, 3 << 20},
    {"ext4-4k", 6, 3 << 20},
};

enum class Change
{
    Extend,
    Overwrite,
    TruncateToSome,
    TruncateToNothing,
    Remove,
    Rename,
};

constexpr std::string_view ChangeNames[] = {"extend",        "overwrite", "truncate to 1000",
                                            "truncate to 0", "remove",    "rename onto"};

constexpr std::size_t Written = 2 << 20;

// A piece of a write that crosses into the double indirect range allocates two indirect blocks
// and a data block besides the inode, the bitmap, the descriptors and the superblock: two blocks
// more than a piece that frees.
std::uint32_t HardFor(const Limits& volume, Change change)
{
    return change == Change::Extend || change == Change::Overwrite ? volume.hard + 2 : volume.hard;
}

Status Perform(Filesystem& ext, Change change)
{
    switch (change)
    {
    case Change::Extend:
    {
        const Result<std::size_t> written =
            ext.Write(Value(Walk(ext, "w.bin")), 0, BytesOfText(PatternAt(0, Written, 3)));
        return written ? Status{} : std::unexpected(written.error());
    }
    case Change::Overwrite:
    {
        const Result<std::size_t> written =
            ext.Write(Value(Walk(ext, "hole.bin")), 0, BytesOfText(PatternAt(0, Written, 3)));
        return written ? Status{} : std::unexpected(written.error());
    }
    case Change::TruncateToSome:
        return ext.Truncate(Value(Walk(ext, "big.bin")), 1000);
    case Change::TruncateToNothing:
        return ext.Truncate(Value(Walk(ext, "big.bin")), 0);
    case Change::Remove:
        return ext.Remove(ext.Root(), "big.bin");
    case Change::Rename:
        return ext.Rename(ext.Root(), "src.txt", ext.Root(), "big.bin");
    }
    return {};
}

// What the volume must hold when the change is done, as text for the comparison.
void RequireDone(Filesystem& ext, Change change)
{
    switch (change)
    {
    case Change::Extend:
    {
        const NodeId file = Value(Walk(ext, "w.bin"));
        const std::string got = ReadAll(ext, file);
        const std::string want = PatternAt(0, Written, 3);
        std::size_t at = 0;
        while (at < got.size() && at < want.size() && got[at] == want[at])
        {
            ++at;
        }
        INFO("size " << got.size() << " first difference at " << at);
        REQUIRE(got == want);
        break;
    }
    case Change::Overwrite:
    {
        const NodeId file = Value(Walk(ext, "hole.bin"));
        REQUIRE(ReadAll(ext, file) == PatternAt(0, Written, 3) + std::string(1 << 20, '\0'));
        break;
    }
    case Change::TruncateToSome:
        REQUIRE(ReadAll(ext, Value(Walk(ext, "big.bin"))) == PatternAt(0, 1000));
        break;
    case Change::TruncateToNothing:
        REQUIRE(ReadAll(ext, Value(Walk(ext, "big.bin"))).empty());
        break;
    case Change::Remove:
        REQUIRE(ErrorOf(Walk(ext, "big.bin")) == Error::NotFound);
        break;
    case Change::Rename:
        REQUIRE(ErrorOf(Walk(ext, "src.txt")) == Error::NotFound);
        REQUIRE(ReadAll(ext, Value(Walk(ext, "big.bin"))) == "src");
        break;
    }
}

enum class Outcome
{
    Done,
    NotDone,
    Other,
};

// Whether a change of the crash tests (which cut big.bin or w.bin) is whole or not at all there.
Outcome Classify(Filesystem& ext, Change change, std::size_t big)
{
    const auto contents = [&](std::string_view name) -> std::optional<std::string>
    {
        const Result<NodeId> node = Walk(ext, name);
        if (!node)
        {
            return std::nullopt;
        }
        return ReadAll(ext, *node);
    };
    const std::optional<std::string> source = contents("src.txt");
    const std::optional<std::string> large = contents("big.bin");
    const std::string whole = PatternAt(0, big);
    switch (change)
    {
    case Change::Extend:
    {
        const std::optional<std::string> written = contents("w.bin");
        if (written && written->empty())
        {
            return Outcome::NotDone;
        }
        return written && *written == PatternAt(0, Written, 3) ? Outcome::Done : Outcome::Other;
    }
    case Change::TruncateToNothing:
        if (large && *large == whole)
        {
            return Outcome::NotDone;
        }
        return large && large->empty() ? Outcome::Done : Outcome::Other;
    case Change::Remove:
        if (large && *large == whole)
        {
            return Outcome::NotDone;
        }
        return !large ? Outcome::Done : Outcome::Other;
    case Change::Rename:
        if (source && large && *large == whole)
        {
            return Outcome::NotDone;
        }
        return !source && large && *large == "src" ? Outcome::Done : Outcome::Other;
    default:
        return Outcome::Other;
    }
}

} // namespace

TEST_CASE("changes too big for one transaction are split, and finish")
{
    for (const Limits& volume : SplitVolumes)
    {
        std::uint32_t splits = 0;
        std::uint32_t added = 0;
        // The 2 MiB a write adds would not fit next to the big file.
        const std::unique_ptr<MemoryDevice> base = DeviceFrom(LoadExtImage(volume.image));
        const std::unique_ptr<MemoryDevice> empty = DeviceFrom(LoadExtImage(volume.image));
        Prepare(*base, volume.big);
        Prepare(*empty, 0);
        for (const Change change : {Change::Extend, Change::Overwrite, Change::TruncateToSome,
                                    Change::TruncateToNothing, Change::Remove, Change::Rename})
        {
            INFO(volume.image << ": " << ChangeNames[static_cast<std::size_t>(change)]);
            const std::unique_ptr<MemoryDevice> device =
                CopyOf(change == Change::Extend || change == Change::Overwrite ? *empty : *base);
            std::uint32_t before = 0;
            {
                Mounted ext(*device);
                before = FreeBlocks(*ext);
                Core(*ext).LimitJournal(
                    {.soft = 3, .hard = HardFor(volume, change), .committed = 32});
                Succeeds(Perform(*ext, change));
                RequireDone(*ext, change);
                const JournalActivity activity = ActivityOf(*ext);
                splits += activity.splits;
                added += activity.orphans_added;
                REQUIRE(activity.orphans_processed == 0);
                if (change == Change::TruncateToSome || change == Change::TruncateToNothing ||
                    change == Change::Remove || change == Change::Rename)
                {
                    REQUIRE(FreeBlocks(*ext) > before);
                }
                Succeeds(ext.Unmount());
            }
            RequireNoOrphans(*device, "split-done");
            CheckClean(*device, "split-done");
            Mounted again(*device);
            RequireDone(*again, change);
        }
        INFO(volume.image);
        REQUIRE(splits > 0);
        REQUIRE(added > 0);
    }
}

TEST_CASE("a split change that cannot finish leaves its orphan for the next mount")
{
    // Pieces of two blocks cannot even store a window of one block; the first piece does.
    const std::unique_ptr<MemoryDevice> base = DeviceFrom(LoadExtImage("ext3-groups"));
    Prepare(*base, 5 << 20);
    const std::unique_ptr<MemoryDevice> device = CopyOf(*base);
    {
        Mounted ext(*device);
        Core(*ext).LimitJournal({.soft = 3, .hard = 4, .committed = 32});
        const Status truncated = ext->Truncate(Value(Walk(*ext, "big.bin")), 0);
        REQUIRE_FALSE(truncated);
        REQUIRE(truncated.error() == Error::NoSpace);
        Succeeds(ext.Unmount());
    }
    // The next mount, with room, finishes the truncation.
    {
        Mounted ext(*device);
        REQUIRE(ext->GetCapabilities().writable);
        REQUIRE(ActivityOf(*ext).orphans_processed == 1);
        REQUIRE(ReadAll(*ext, Value(Walk(*ext, "big.bin"))).empty());
        Succeeds(ext.Unmount());
    }
    RequireNoOrphans(*device, "split-unfinished");
    CheckClean(*device, "split-unfinished");
}

TEST_CASE("a crash in the middle of a split leaves the change done or not done")
{
    struct Volume
    {
        Limits limits;
        std::vector<Change> changes;
    };
    const std::vector<Volume> volumes = {
        {SplitVolumes[1],
         {Change::Extend, Change::TruncateToNothing, Change::Remove, Change::Rename}},
        {SplitVolumes[0], {Change::Extend, Change::Remove, Change::Rename}},
    };
    for (const Volume& chosen : volumes)
    {
        const Limits& volume = chosen.limits;
        bool pending_seen = false;
        const std::unique_ptr<MemoryDevice> whole = DeviceFrom(LoadExtImage(volume.image));
        const std::unique_ptr<MemoryDevice> empty = DeviceFrom(LoadExtImage(volume.image));
        Prepare(*whole, volume.big);
        Prepare(*empty, 0);
        for (const Change change : chosen.changes)
        {
            MemoryDevice* const base = change == Change::Extend ? empty.get() : whole.get();
            INFO(volume.image << ": " << ChangeNames[static_cast<std::size_t>(change)]);
            const JournalLimits limits = {
                .soft = 3, .hard = HardFor(volume, change), .committed = 32};
            // Where the commits of an uninterrupted run reached the device.
            std::uint64_t start = 0;
            std::vector<std::uint64_t> marks;
            {
                const std::unique_ptr<MemoryDevice> device = CopyOf(*base);
                Mounted ext(*device);
                Core(*ext).LimitJournal(limits);
                start = device->write_count;
                device->flush_writes.clear();
                Succeeds(Perform(*ext, change));
                Succeeds(ext->Sync());
                marks = device->flush_writes;
            }
            REQUIRE(marks.size() >= 4);
            const std::size_t last = marks.size() - 1;
            std::size_t middle = last / 2;
            middle -= middle % 2;
            for (const std::uint64_t cut : {marks[1], marks[middle], marks[last - 1], marks[last]})
            {
                const std::unique_ptr<MemoryDevice> crashed = CopyOf(*base);
                {
                    Mounted ext(*crashed);
                    Core(*ext).LimitJournal(limits);
                    crashed->FailWritesAfter(crashed->write_count + (cut - start));
                    static_cast<void>(Perform(*ext, change));
                }
                // After the journal alone, the entry is pending in some of the cuts. e2fsck -fn
                // does not finish it (only a repairing run does), so what it says is not asserted.
                const std::unique_ptr<MemoryDevice> journalled = CopyOf(*crashed);
                if (ChangeWithDebugfs(*journalled, "split-jr", "jr").ran)
                {
                    const OrphanState state = StateOf(*journalled, "split-state");
                    pending_seen = pending_seen || state.last_orphan != 0 || !state.slots.empty();
                }
                const std::unique_ptr<MemoryDevice> ours = CopyOf(*crashed);
                {
                    Mounted ext(*ours);
                    REQUIRE(ext->GetCapabilities().writable);
                    Succeeds(ext.Unmount());
                }
                RequireNoOrphans(*ours, "split-ours");
                CheckClean(*ours, "split-ours");
                {
                    Mounted ext(*ours, true);
                    REQUIRE(Classify(*ext, change, volume.big) != Outcome::Other);
                }
                const std::unique_ptr<MemoryDevice> theirs = CopyOf(*crashed);
                const ToolRun fixed = FixWithE2fsck(*theirs, "split-theirs");
                if (fixed.ran)
                {
                    INFO(fixed.output);
                    REQUIRE(fixed.status <= 1);
                    REQUIRE(fixed.output.find("hecksum") == std::string::npos);
                    REQUIRE(fixed.output.find("not clean") == std::string::npos);
                    CheckClean(*theirs, "split-theirs");
                    const std::string tree =
                        FirstDifference(SnapshotOf(*ours), SnapshotOf(*theirs));
                    INFO(tree);
                    REQUIRE(tree.empty());
                    const std::string free =
                        FirstDifference(FreeRangesAfterFix(*ours, "split-free-ours"),
                                        FreeRangesAfterFix(*theirs, "split-free-theirs"));
                    INFO(free);
                    REQUIRE(free.empty());
                }
            }
        }
        INFO(volume.image);
        REQUIRE((pending_seen || !HaveE2fsck()));
    }
}

TEST_CASE("no orphan entry is left after any change has been committed")
{
    std::uint32_t splits = 0;
    for (const Limits& volume : SplitVolumes)
    {
        INFO(volume.image);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(volume.image));
        Mounted ext(*device);
        const auto committed = [&](std::string_view name)
        {
            Succeeds(ext->Sync());
            const std::unique_ptr<MemoryDevice> copy = CopyOf(*device);
            if (ChangeWithDebugfs(*copy, "orphan-committed", "jr").ran)
            {
                INFO(name);
                RequireNoOrphans(*copy, "orphan-committed");
            }
        };
        const NodeId root = ext->Root();
        const NodeId directory = Value(ext->MakeDirectory(root, "d", 0755));
        committed("mkdir");
        const NodeId file = Value(ext->Create(directory, "f", 0644));
        committed("create");
        // Changes that split under these limits, and those that need no more than they allow.
        Core(*ext).LimitJournal({.soft = 3, .hard = volume.hard + 2, .committed = 32});
        REQUIRE(Value(ext->Write(file, 0, BytesOfText(PatternAt(0, 1 << 20)))) == (1 << 20));
        committed("write");
        Succeeds(ext->Truncate(file, 5000));
        committed("truncate");
        REQUIRE(Value(ext->Write(file, 0, BytesOfText(PatternAt(0, 1 << 20, 7)))) == (1 << 20));
        committed("write again");
        Succeeds(ext->Truncate(file, 0));
        committed("truncate to nothing");
        const NodeId other = Value(ext->Create(directory, "g", 0644));
        REQUIRE(Value(ext->Write(other, 0, BytesOfText(PatternAt(0, 1 << 19)))) == (1 << 19));
        committed("write to another");
        Succeeds(ext->Remove(directory, "g"));
        committed("remove");
        Succeeds(ext->Remove(directory, "f"));
        committed("remove an empty file");
        Succeeds(ext->RemoveDirectory(root, "d"));
        committed("rmdir");
        splits += ActivityOf(*ext).splits;
        Succeeds(ext.Unmount());
        CheckClean(*device, "orphan-committed-end");
    }
    REQUIRE(splits > 0);
}
