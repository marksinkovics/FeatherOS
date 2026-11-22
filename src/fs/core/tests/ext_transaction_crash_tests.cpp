// SPDX-License-Identifier: MIT OR Apache-2.0
// What a crash leaves on ext3 and ext4 volumes written through the journal: runs stopped after
// every write (or every write of the interesting stretches), recovered by our own writable mount
// and by e2fsck -fy, must hold the tree of a committed state and nothing of a stale block.
#include <catch2/catch_test_macros.hpp>

#include "ext/ext2_filesystem.h"
#include "fs/ext2.h"
#include "tests/crash_helpers.h"
#include "tests/crash_scenarios.h"
#include "tests/ext2_image.h"
#include "tests/filesystem_helpers.h"
#include "tests/memory_device.h"
#include "tests/page_cache_support.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using namespace FeatherDisk;
using namespace FeatherDisk::Tests;
using namespace FeatherDisk::Tests::Crash;
using Ext::Ext2Type;

// The two default layouts are cut at every write; the others, which differ in the log's format
// and checksums, at every third. One test case each, so that CTest runs them side by side.
[[maybe_unused]] void StopAfterEveryWrite(std::string_view variant, std::uint64_t stride,
                                          std::uint64_t first)
{
    INFO(variant);
    const Scenario scenario = MixedScenario();
    const std::unique_ptr<MemoryDevice> base = Prepared(variant);
    const Reference reference = RunReference(*base, scenario, std::nullopt);
    const std::size_t block_size = BlockSizeOf(*base);
    REQUIRE(reference.total > 200);
    for (std::uint64_t cut = first; cut < reference.total; cut += stride)
    {
        INFO("stopped after write " << cut << " of " << reference.total);
        const Crashed crashed = RunCrashed(*base, scenario, cut, std::nullopt);
        const Replayed replayed = ReplayBothWays(*crashed.device, "crash-every-write");
        RequireCommittedState(replayed.tree, reference.trees, crashed.synced, block_size);
        RequireNoStaleFill(replayed.tree);
        RequireLossyRecovery(crashed, cut, reference.trees, block_size);
    }
}

} // namespace

TEST_CASE("a journalled volume stopped after any write recovers to a committed state: ext3")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    StopAfterEveryWrite("ext3", 1, 0);
#endif
}

TEST_CASE("a journalled volume stopped after any write recovers to a committed state: ext4")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    StopAfterEveryWrite("ext4", 1, 0);
#endif
}

TEST_CASE(
    "a journalled volume stopped after any write recovers to a committed state: ext4-blockmap-v2")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    StopAfterEveryWrite("ext4-blockmap-v2", 3, 1);
#endif
}

TEST_CASE("a journalled volume stopped after any write recovers to a committed state: "
          "ext4-journal-gdtcsum")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    StopAfterEveryWrite("ext4-journal-gdtcsum", 3, 2);
#endif
}

TEST_CASE("a journalled volume stopped after any write recovers to a committed state: ext4-64bit")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    StopAfterEveryWrite("ext4-64bit", 3, 0);
#endif
}

TEST_CASE(
    "a volume stopped around the log's wrap and its checkpoints recovers to a committed state")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const std::string_view variant : {"ext3", "ext4"})
    {
        INFO(variant);
        const std::unique_ptr<MemoryDevice> base = Prepared(variant);
        const std::size_t block_size = BlockSizeOf(*base);
        const Scenario scenario = CycleScenario(150);
        const Reference reference = RunReference(*base, scenario, std::nullopt);
        const std::size_t checkpoint = FirstStepWhere(
            reference, [](const JournalActivity& before, const JournalActivity& after)
            { return after.checkpoints > before.checkpoints; });
        const std::size_t wrap = FirstStepWhere(
            reference, [](const JournalActivity& before, const JournalActivity& after)
            { return after.wraps > before.wraps; });
        REQUIRE(checkpoint >= 2);
        REQUIRE(wrap >= 2);
        // The commits from the one before the first checkpoint or wrap to the one after.
        const std::size_t first = std::min(checkpoint, wrap) - 1;
        const std::size_t last = std::max(checkpoint, wrap) + 1;
        std::vector<std::uint64_t> cuts =
            CutsBetween(reference.writes[first - 1], reference.writes[last], 400);
        const std::vector<std::uint64_t> unmount =
            CutsBetween(reference.writes.back(), reference.total, 400);
        cuts.insert(cuts.end(), unmount.begin(), unmount.end());
        INFO("checkpoint at change " << checkpoint << ", wrap at " << wrap);
        for (const std::uint64_t cut : cuts)
        {
            INFO("stopped after write " << cut << " of " << reference.total);
            const Crashed crashed = RunCrashed(*base, scenario, cut, std::nullopt);
            const Replayed replayed = ReplayBothWays(*crashed.device, "crash-wrap");
            RequireCommittedState(replayed.tree, reference.trees, crashed.synced, block_size);
            RequireNoStaleFill(replayed.tree);
            RequireLossyRecovery(crashed, cut, reference.trees, block_size);
        }
    }
#endif
}

TEST_CASE("a split change stopped after any write is done or not done once recovered")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    std::uint32_t pending = 0;
    for (const SplitVolume& volume : SplitVolumes)
    {
        const std::unique_ptr<MemoryDevice> with_big = PreparedForSplit(volume, true);
        const std::unique_ptr<MemoryDevice> without_big = PreparedForSplit(volume, false);
        const std::size_t block_size = BlockSizeOf(*with_big);
        std::uint32_t splits = 0;
        std::uint32_t added = 0;
        for (const Split split : {Split::Extend, Split::TruncateToSome, Split::TruncateToNothing,
                                  Split::Remove, Split::Rename})
        {
            INFO(volume.image << ": " << SplitNames[static_cast<std::size_t>(split)]);
            MemoryDevice* const base = split == Split::Extend ? without_big.get() : with_big.get();
            const Scenario scenario = {SplitChange(split)};
            const JournalLimits limits = SplitLimits(volume, split);
            const Reference reference = RunReference(*base, scenario, limits);
            REQUIRE_FALSE(TreeDifference(reference.trees[0], reference.trees[1]).empty());
            splits += reference.activity.back().splits;
            added += reference.activity.back().orphans_added;
            // Every write of the change and its commits; the unmount only checkpoints.
            const std::vector<std::uint64_t> cuts =
                CutsAroundFlushes(reference, reference.mounted, reference.writes[1] + 1, 40);
            INFO(cuts.size() << " cuts");
            const std::uint32_t replaced_inode =
                split == Split::Rename
                    ? InodeOfPath(*base, "crash-split-old", "/big.bin").value_or(0)
                    : 0;
            const std::uint32_t moving_inode =
                split == Split::Rename
                    ? InodeOfPath(*base, "crash-split-new", "/src.txt").value_or(0)
                    : 0;
            std::uint32_t released = 0;
            for (const std::uint64_t cut : cuts)
            {
                INFO("stopped after write " << cut << " of " << reference.writes[1]);
                const Crashed crashed = RunCrashed(*base, scenario, cut, limits);
                const Replayed replayed = ReplayBothWays(*crashed.device, "crash-split");
                pending += replayed.orphans;
                RequireCommittedState(replayed.tree, reference.trees, 0, block_size);
                if (split == Split::Rename)
                {
                    // The target name is never missing, and a replaced inode no longer named is
                    // free once recovered (e2fsck's recovery holds the same free ranges).
                    REQUIRE(replaced_inode != 0);
                    REQUIRE(moving_inode != 0);
                    const std::optional<std::uint32_t> named =
                        InodeOfPath(*replayed.ours, "crash-split-target", "/big.bin");
                    REQUIRE(named.has_value());
                    REQUIRE((*named == replaced_inode || *named == moving_inode));
                    if (*named == moving_inode)
                    {
                        const ToolRun used =
                            RunDebugfs(*replayed.ours, "crash-split-used",
                                       "testi <" + std::to_string(replaced_inode) + ">");
                        INFO(used.output);
                        REQUIRE(used.output.find("not in use") != std::string::npos);
                        ++released;
                    }
                }
            }
            REQUIRE((split != Split::Rename || released > 0));
        }
        INFO(volume.image);
        REQUIRE(splits > 0);
        REQUIRE(added > 0);
    }
    REQUIRE(pending > 0);
#endif
}

TEST_CASE("an interrupted replay or orphan processing is finished by the next mount")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    const SplitVolume& volume = SplitVolumes[1];
    const std::unique_ptr<MemoryDevice> base = PreparedForSplit(volume, true);
    const Scenario scenario = {SplitChange(Split::Extend)};
    const JournalLimits limits = SplitLimits(volume, Split::Extend);
    const Reference reference = RunReference(*base, scenario, limits, false);
    // Crash images with two or more transactions in the log and an orphan entry pending.
    std::vector<std::unique_ptr<MemoryDevice>> qualifying;
    const std::vector<std::uint64_t> candidates =
        CutsBetween(reference.mounted, reference.writes[1] + 1, 60);
    for (const std::uint64_t cut : candidates)
    {
        const Crashed crashed = RunCrashed(*base, scenario, cut, limits);
        const std::string log =
            RunDebugfs(*crashed.device, "crash-interrupted-log", "logdump").output;
        std::size_t commits = 0;
        for (std::size_t at = log.find("commit block"); at != std::string::npos;
             at = log.find("commit block", at + 1))
        {
            ++commits;
        }
        if (commits < 2)
        {
            continue;
        }
        const std::unique_ptr<MemoryDevice> probe = CopyOf(*crashed.device);
        Filesystem* filesystem = Value(Ext2Type().Mount(Mountable(*probe), {.read_only = false}));
        const std::uint32_t orphans = ActivityOf(*filesystem).orphans_processed;
        Succeeds(Ext2Type().Unmount(*filesystem));
        if (orphans > 0)
        {
            qualifying.push_back(CopyOf(*crashed.device));
        }
    }
    REQUIRE(qualifying.size() >= 5);
    // Five of them, from the first to the last.
    std::vector<std::unique_ptr<MemoryDevice>> images;
    for (std::size_t index = 0; index < 5; ++index)
    {
        images.push_back(std::move(qualifying[index * (qualifying.size() - 1) / 4]));
    }
    for (const std::unique_ptr<MemoryDevice>& image : images)
    {
        const std::unique_ptr<MemoryDevice> whole = CopyOf(*image);
        Filesystem* filesystem = Value(Ext2Type().Mount(Mountable(*whole), {.read_only = false}));
        const std::uint64_t mount_writes = whole->write_count;
        Succeeds(Ext2Type().Unmount(*filesystem));
        const Tree tree = TreeOf(*whole);
        const std::string free_ranges = FreeRangesOf(*whole, "crash-interrupted-free");
        REQUIRE(mount_writes > 4);
        for (std::uint64_t cut = 0; cut < mount_writes; ++cut)
        {
            INFO("mount stopped after write " << cut << " of " << mount_writes);
            const std::unique_ptr<MemoryDevice> stopped = CopyOf(*image);
            stopped->FailWritesAfter(cut);
            const Result<Filesystem*> mounted =
                Ext2Type().Mount(Mountable(*stopped), {.read_only = false});
            if (mounted)
            {
                stopped->FailWritesAfter(UINT64_MAX);
                Succeeds(Ext2Type().Unmount(**mounted));
            }
            stopped->FailWritesAfter(UINT64_MAX);
            ReplayByMounting(*stopped);
            CheckClean(*stopped, "crash-interrupted");
            const std::string difference = TreeDifference(TreeOf(*stopped), tree);
            INFO(difference);
            REQUIRE(difference.empty());
            REQUIRE(FreeRangesOf(*stopped, "crash-interrupted-free") == free_ranges);
        }
    }
#endif
}

namespace
{

// The trees after k and after k + 1 changes of the scenario.
std::vector<Tree> TreesAround(MemoryDevice& base, const Scenario& scenario, std::size_t synced,
                              std::optional<JournalLimits> limits)
{
    std::vector<Tree> trees;
    if (synced == 0)
    {
        trees.push_back(TreeOf(base));
    }
    const std::unique_ptr<MemoryDevice> device = CopyOf(base);
    Filesystem* filesystem = Value(Ext2Type().Mount(Mountable(*device), {.read_only = false}));
    if (limits)
    {
        Core(*filesystem).LimitJournal(*limits);
    }
    std::size_t skipped = 0;
    Run(
        *filesystem, scenario,
        [&](Filesystem& live, std::size_t done)
        {
            if (done == synced || done == synced + 1)
            {
                trees.push_back(TreeOf(live));
            }
        },
        &skipped);
    Succeeds(Ext2Type().Unmount(*filesystem));
    if (trees.size() == 1)
    {
        trees.push_back(trees[0]);
    }
    return trees;
}

} // namespace

TEST_CASE("random changes on ext3 and ext4 stopped at a random write recover to a committed state")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    std::uint32_t splits = 0;
    std::size_t skipped = 0;
    for (const std::string_view variant : {"ext4", "ext3"})
    {
        for (std::uint32_t seed = 1; seed <= 8; ++seed)
        {
            const bool limited = seed % 2 == 0;
            INFO(variant << " seed " << seed << (limited ? " with limits" : ""));
            // Limits that commit by memory and checkpoint often, and limits so small that
            // changes split.
            const std::unique_ptr<MemoryDevice> base = Prepared(variant);
            const std::size_t block_size = BlockSizeOf(*base);
            const Scenario scenario = RandomScenario(seed * 7919, 300, limited);
            const std::optional<JournalLimits> limits =
                !limited        ? std::nullopt
                : seed % 4 == 2 ? std::optional<JournalLimits>(
                                      JournalLimits{.soft = 8, .hard = 24, .committed = 32})
                                : std::optional<JournalLimits>(
                                      JournalLimits{.soft = 3, .hard = 8, .committed = 32});
            const Reference reference = RunReference(*base, scenario, limits, false, limited);
            splits += reference.activity.back().splits;
            skipped += reference.skipped;
            std::mt19937 choose(seed);
            for (int attempt = 0; attempt < 4; ++attempt)
            {
                const std::uint64_t cut = choose() % reference.total;
                INFO("stopped after write " << cut << " of " << reference.total);
                const Crashed crashed = RunCrashed(*base, scenario, cut, limits, limited);
                const Replayed replayed = ReplayBothWays(*crashed.device, "crash-random");
                const std::vector<Tree> trees =
                    TreesAround(*base, scenario, crashed.synced, limits);
                RequireCommittedState(replayed.tree, trees, 0, block_size);
            }
        }
    }
    INFO("splits " << splits << ", skipped " << skipped);
    REQUIRE(splits > 0);
#endif
}
