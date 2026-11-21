// SPDX-License-Identifier: MIT OR Apache-2.0
// A rename onto an existing name, stopped after every write: the target name always exists and
// names the old inode or the new one. On ext3 and ext4 recovery needs no repair; on a volume
// without a journal e2fsck finds only routine leftovers.
#include <catch2/catch_test_macros.hpp>

#include "ext/ext2_filesystem.h"
#include "ext/inode.h"
#include "fs/ext2.h"
#include "tests/crash_helpers.h"
#include "tests/crash_scenarios.h"
#include "tests/ext2_image.h"
#include "tests/filesystem_helpers.h"
#include "tests/memory_device.h"
#include "tests/page_cache_support.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef FEATHERDISK_EXT_IMAGES
namespace
{

using namespace FeatherDisk;
using namespace FeatherDisk::Tests;
using namespace FeatherDisk::Tests::Crash;
using Ext::Ext2Type;
using Ext::InodeIndexed;

std::unique_ptr<MemoryDevice> FinalImage(MemoryDevice& base, const Scenario& scenario)
{
    std::unique_ptr<MemoryDevice> device = CopyOf(base);
    Filesystem* filesystem = Value(Ext2Type().Mount(Mountable(*device), {.read_only = false}));
    REQUIRE(Run(*filesystem, scenario) == scenario.size());
    Succeeds(Ext2Type().Unmount(*filesystem));
    return device;
}

std::uint32_t InodeAt(MemoryDevice& device, std::string_view path)
{
    const std::optional<std::uint32_t> inode = InodeOfPath(device, "rename-inode", path);
    INFO(path);
    REQUIRE(inode.has_value());
    return *inode;
}

// What each target's inode may be: the one it had, or the source's that replaces it.
struct Inodes
{
    std::array<std::uint32_t, Replacements.size()> old_inode{};
    std::array<std::uint32_t, Replacements.size()> new_inode{};
};

Inodes InodesOf(MemoryDevice& base, MemoryDevice& final_image)
{
    Inodes inodes;
    for (std::size_t index = 0; index < Replacements.size(); ++index)
    {
        inodes.old_inode[index] = InodeAt(base, Replacements[index].target);
        inodes.new_inode[index] = index == MadeInScenario
                                      ? InodeAt(final_image, Replacements[index].target)
                                      : InodeAt(base, Replacements[index].source);
    }
    return inodes;
}

struct Seen
{
    std::array<bool, Replacements.size()> old_inode{};
    std::array<bool, Replacements.size()> new_inode{};
};

void RequireTargetsOldOrNew(MemoryDevice& image, const Inodes& inodes, Seen& seen)
{
    for (std::size_t index = 0; index < Replacements.size(); ++index)
    {
        const std::optional<std::uint32_t> inode =
            InodeOfPath(image, "rename-target", Replacements[index].target);
        INFO("target " << Replacements[index].target);
        REQUIRE(inode.has_value());
        INFO("inode " << *inode << ", old " << inodes.old_inode[index] << ", new "
                      << inodes.new_inode[index]);
        REQUIRE((*inode == inodes.old_inode[index] || *inode == inodes.new_inode[index]));
        seen.old_inode[index] = seen.old_inode[index] || *inode == inodes.old_inode[index];
        seen.new_inode[index] = seen.new_inode[index] || *inode == inodes.new_inode[index];
    }
}

void RequireSeenBoth(const Seen& seen, const Inodes& inodes)
{
    for (std::size_t index = 0; index < Replacements.size(); ++index)
    {
        if (inodes.old_inode[index] == inodes.new_inode[index])
        {
            continue;
        }
        INFO("target " << Replacements[index].target);
        REQUIRE(seen.old_inode[index]);
        REQUIRE(seen.new_inode[index]);
    }
}

void ReplacingCrashesOnJournalled(std::string_view variant, std::uint64_t stride)
{
    INFO(variant);
    const Scenario scenario = RenameScenario();
    const std::unique_ptr<MemoryDevice> base = Fixture(variant, FixtureSetup());
    const Reference reference = RunReference(*base, scenario, std::nullopt);
    const std::size_t block_size = BlockSizeOf(*base);
    const Inodes inodes = InodesOf(*base, *reference.final_device);
    REQUIRE(reference.trees.size() == scenario.size() + 1);
    Seen seen;
    std::uint64_t cuts = 0;
    for (std::uint64_t cut = 0; cut < reference.total; cut += stride)
    {
        INFO("stopped after write " << cut << " of " << reference.total);
        const Crashed crashed = RunCrashed(*base, scenario, cut, std::nullopt);
        const Replayed replayed = ReplayBothWays(*crashed.device, "rename-crash");
        RequireCommittedState(replayed.tree, reference.trees, crashed.synced, block_size);
        RequireLossyRecovery(crashed, cut, reference.trees, block_size);
        RequireTargetsOldOrNew(*replayed.ours, inodes, seen);
        ++cuts;
    }
    INFO(cuts << " cuts");
    RequireSeenBoth(seen, inodes);
}

} // namespace
#endif

TEST_CASE("a replacing rename stopped after any write leaves the old or the new target: ext3")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    ReplacingCrashesOnJournalled("ext3", 1);
#endif
}

TEST_CASE("a replacing rename stopped after any write leaves the old or the new target: ext4")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    ReplacingCrashesOnJournalled("ext4", 1);
#endif
}

TEST_CASE("a replacing rename stopped after any write leaves the old or the new target: ext4-64bit")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    ReplacingCrashesOnJournalled("ext4-64bit", 1);
#endif
}

#ifdef FEATHERDISK_EXT_IMAGES
namespace
{

bool Mentions(const std::string& report, std::string_view phrase, std::uint32_t inode)
{
    const std::string wanted = std::string(phrase) + " " + std::to_string(inode);
    for (std::size_t at = report.find(wanted); at != std::string::npos;
         at = report.find(wanted, at + 1))
    {
        const std::size_t end = at + wanted.size();
        if (end >= report.size() || report[end] < '0' || report[end] > '9')
        {
            return true;
        }
    }
    return false;
}

constexpr std::size_t DirectoryCase = 3;

// A directory with two names is left with one by e2fsck -fy, which keeps the first it finds.
void RequireRepaired(MemoryDevice& image, const Inodes& inodes)
{
    for (std::size_t index = 0; index < Replacements.size(); ++index)
    {
        const std::optional<std::uint32_t> inode =
            InodeOfPath(image, "rename-repaired", Replacements[index].target);
        INFO("target " << Replacements[index].target);
        if (!inode && index == DirectoryCase)
        {
            REQUIRE(InodeAt(image, Replacements[index].source) == inodes.new_inode[index]);
            continue;
        }
        REQUIRE(inode.has_value());
        REQUIRE((*inode == inodes.old_inode[index] || *inode == inodes.new_inode[index]));
    }
}

std::string CatOf(MemoryDevice& image, std::string_view path)
{
    return InspectWithDebugfs(image, "rename-cat", "cat " + std::string(path)).output;
}

void ReplacingCrashesWithoutJournal(std::string_view variant, std::uint64_t stride)
{
    INFO(variant);
    if (!HaveE2fsck())
    {
        SKIP("e2fsck is not installed");
    }
    const Scenario scenario = RenameScenario();
    const std::unique_ptr<MemoryDevice> base = Fixture(variant, FixtureSetup());
    const std::unique_ptr<MemoryDevice> final_image = FinalImage(*base, scenario);
    const Inodes inodes = InodesOf(*base, *final_image);
    const Tree before = TreeOf(*base);
    const Tree after = TreeOf(*final_image);
    std::uint64_t total = 0;
    std::uint64_t mounted_writes = 0;
    {
        const std::unique_ptr<MemoryDevice> device = CopyOf(*base);
        Filesystem* filesystem = Value(Ext2Type().Mount(Mountable(*device), {.read_only = false}));
        mounted_writes = device->write_count;
        Run(*filesystem, scenario);
        Succeeds(Ext2Type().Unmount(*filesystem));
        total = device->write_count;
    }
    REQUIRE(total > mounted_writes + 50);
    Seen seen;
    bool unattached = false;
    std::uint64_t cuts = 0;
    for (std::uint64_t cut = mounted_writes; cut < total; cut += stride)
    {
        INFO("stopped after write " << cut << " of " << total);
        const std::unique_ptr<MemoryDevice> device = CopyOf(*base);
        device->FailWritesAfter(cut);
        const Result<Filesystem*> mounted =
            Ext2Type().Mount(Mountable(*device), {.read_only = false});
        REQUIRE(mounted.has_value());
        Run(**mounted, scenario);
        const std::unique_ptr<MemoryDevice> stopped = CopyOf(*device);
        device->FailWritesAfter(UINT64_MAX);
        Succeeds(Ext2Type().Unmount(**mounted));
        ++cuts;

        const ToolRun report = RunE2fsck(*stopped, "rename-nojournal");
        INFO(report.output);
        REQUIRE(OnlyRoutineRepairs(report.output, true));
        // Its "Clear? no" is on OnlyRoutineRepairs's list of noise.
        REQUIRE(report.output.find("deleted/unused") == std::string::npos);
        RequireTargetsOldOrNew(*stopped, inodes, seen);
        for (std::size_t index = 0; index < Replacements.size(); ++index)
        {
            const std::string target(Replacements[index].target);
            INFO("target " << target);
            const std::uint32_t inode = InodeAt(*stopped, target);
            const bool is_new = inode == inodes.new_inode[index];
            if (index != MadeInScenario && !is_new)
            {
                // Until the new record is written the moving inode keeps its own name.
                REQUIRE(InodeAt(*stopped, Replacements[index].source) == inodes.new_inode[index]);
            }
            if (before.at(target).type == NodeType::File && after.at(target).type == NodeType::File)
            {
                const std::string& expected =
                    is_new ? after.at(target).contents : before.at(target).contents;
                REQUIRE(CatOf(*stopped, target) == expected);
            }
            if (is_new && index != HardLinkCase &&
                (Mentions(report.output, "Unattached inode", inodes.old_inode[index]) ||
                 Mentions(report.output, "Unconnected directory inode", inodes.old_inode[index])))
            {
                unattached = true;
            }
        }
        const std::unique_ptr<MemoryDevice> fixed = CopyOf(*stopped);
        REQUIRE(FixWithE2fsck(*fixed, "rename-nojournal-fixed").status <= 1);
        CheckClean(*fixed, "rename-nojournal-fixed");
        RequireRepaired(*fixed, inodes);
    }
    INFO(cuts << " cuts");
    RequireSeenBoth(seen, inodes);
    REQUIRE(unattached);
}

} // namespace
#endif

TEST_CASE("a replacing rename on ext2 without a journal leaves the old or the new target: 1k")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    ReplacingCrashesWithoutJournal("1k", 1);
#endif
}

TEST_CASE(
    "a replacing rename on ext2 without a journal leaves the old or the new target: ext4-nojournal")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    ReplacingCrashesWithoutJournal("ext4-nojournal", 1);
#endif
}

TEST_CASE("a replacing rename on ext2 without a journal leaves the old or the new target: "
          "ext4-csum")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    ReplacingCrashesWithoutJournal("ext4-csum", 1);
#endif
}

#ifdef FEATHERDISK_EXT_IMAGES
namespace
{

std::string HashedName(int number)
{
    std::array<char, 64> name{};
    std::snprintf(name.data(), name.size(), "file-with-a-long-name-for-htree-%05d.txt", number);
    return name.data();
}

std::uint32_t InodeFlagsOf(MemoryDevice& device, std::uint32_t inode)
{
    std::vector<std::uint8_t> bytes = BytesOf(device);
    RawVolume raw{bytes};
    return raw.Load32(raw.InodeOffset(inode) + 32);
}

} // namespace
#endif

TEST_CASE("a replacing rename in a hashed directory keeps its index at every write")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    const std::string target = "/large/" + HashedName(1);
    const Scenario setup = {Make("/r.txt", "replacing\n")};
    const Scenario scenario = {Move("/r.txt", target)};
    const std::unique_ptr<MemoryDevice> base = Fixture("ext4-htree", setup);
    const std::uint32_t old_inode = InodeAt(*base, target);
    const std::uint32_t new_inode = InodeAt(*base, "/r.txt");
    const std::uint32_t large = InodeAt(*base, "/large");
    REQUIRE((InodeFlagsOf(*base, large) & InodeIndexed) != 0);
    std::uint64_t total = 0;
    std::uint64_t mounted_writes = 0;
    {
        const std::unique_ptr<MemoryDevice> device = CopyOf(*base);
        Filesystem* filesystem = Value(Ext2Type().Mount(Mountable(*device), {.read_only = false}));
        mounted_writes = device->write_count;
        REQUIRE(Run(*filesystem, scenario) == 1);
        Succeeds(Ext2Type().Unmount(*filesystem));
        total = device->write_count;
        REQUIRE((InodeFlagsOf(*device, large) & InodeIndexed) != 0);
        CheckClean(*device, "rename-htree-complete");
    }
    REQUIRE(total > mounted_writes);
    bool saw_old = false;
    bool saw_new = false;
    for (std::uint64_t cut = mounted_writes; cut <= total; ++cut)
    {
        INFO("stopped after write " << cut << " of " << total);
        const std::unique_ptr<MemoryDevice> device = CopyOf(*base);
        device->FailWritesAfter(cut);
        const Result<Filesystem*> mounted =
            Ext2Type().Mount(Mountable(*device), {.read_only = false});
        REQUIRE(mounted.has_value());
        Run(**mounted, scenario);
        const std::unique_ptr<MemoryDevice> stopped = CopyOf(*device);
        device->FailWritesAfter(UINT64_MAX);
        Succeeds(Ext2Type().Unmount(**mounted));

        const ToolRun report = RunE2fsck(*stopped, "rename-htree");
        INFO(report.output);
        REQUIRE(OnlyRoutineRepairs(report.output, true));
        REQUIRE(report.output.find("deleted/unused") == std::string::npos);
        REQUIRE((InodeFlagsOf(*stopped, large) & InodeIndexed) != 0);
        const ToolRun htree = InspectWithDebugfs(*stopped, "rename-htree-levels", "htree /large");
        REQUIRE(htree.output.find("Indirect levels: 1") != std::string::npos);
        const std::uint32_t inode = InodeAt(*stopped, target);
        REQUIRE((inode == old_inode || inode == new_inode));
        saw_old = saw_old || inode == old_inode;
        saw_new = saw_new || inode == new_inode;
    }
    REQUIRE(saw_old);
    REQUIRE(saw_new);
#endif
}
