// SPDX-License-Identifier: MIT OR Apache-2.0
// The kernel mounts a volume over a CachedDevice, with a block cache that keeps nothing; the
// tools keep a 32 KiB block cache over the bare device. Both must send the device the same
// writes and flushes, so every crash leaves the same bytes and every recovery the same volume.
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
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#ifdef FEATHERDISK_EXT_IMAGES
namespace
{

using namespace FeatherDisk;
using namespace FeatherDisk::Tests;
using namespace FeatherDisk::Tests::Crash;

constexpr std::size_t KernelPages = 4;

bool SameBytes(MemoryDevice& left, MemoryDevice& right)
{
    return left.Bytes().size() == right.Bytes().size() &&
           std::memcmp(left.Bytes().data(), right.Bytes().data(), left.Bytes().size()) == 0;
}

void RequireSameDevices(MemoryDevice& today, MemoryDevice& kernel)
{
    REQUIRE(today.write_count == kernel.write_count);
    REQUIRE(today.flush_writes == kernel.flush_writes);
    REQUIRE(today.call_digest == kernel.call_digest);
    REQUIRE(today.UnflushedCount() == kernel.UnflushedCount());
    REQUIRE(SameBytes(today, kernel));
}

void RequireSameLosses(const Crashed& today, const Crashed& kernel, std::uint64_t cut)
{
    if (today.device->UnflushedCount() < 2)
    {
        return;
    }
    const std::vector<std::function<bool(std::size_t)>> patterns = {
        [](std::size_t) { return false; },
        [](std::size_t) { return true; },
        [](std::size_t index) { return index % 2 == 0; },
        [](std::size_t index) { return index % 3 == 1; },
    };
    for (const auto& keep : patterns)
    {
        const std::unique_ptr<MemoryDevice> left = today.device->CopyKeeping(keep);
        const std::unique_ptr<MemoryDevice> right = kernel.device->CopyKeeping(keep);
        REQUIRE(SameBytes(*left, *right));
    }
    for (std::uint32_t round = 0; round < 2; ++round)
    {
        std::mt19937 first(static_cast<std::uint32_t>(cut) * 2654435761U + round);
        std::mt19937 second(static_cast<std::uint32_t>(cut) * 2654435761U + round);
        const std::unique_ptr<MemoryDevice> left =
            today.device->CopyKeeping([&](std::size_t) { return first() % 2 == 0; });
        const std::unique_ptr<MemoryDevice> right =
            kernel.device->CopyKeeping([&](std::size_t) { return second() % 2 == 0; });
        REQUIRE(SameBytes(*left, *right));
    }
}

// A journalled volume's tree after our recovery is a committed one; a volume without a journal
// has no such promise, so only its bytes are compared.
void RequireRecoversTo(const Crashed& crashed, std::uint64_t cut, const Reference& reference,
                       std::size_t block_size)
{
    const std::unique_ptr<MemoryDevice> copy = CopyOf(*crashed.device);
    ReplayByMounting(*copy);
    RequireCommittedState(TreeOf(*copy), reference.trees, crashed.synced, block_size);
    RequireLossyRecovery(crashed, cut, reference.trees, block_size);
}

struct Compared
{
    std::uint64_t cuts = 0;
    Reference today;
};

Compared CompareRuns(MemoryDevice& base, const Scenario& scenario,
                     const std::optional<JournalLimits>& limits, bool journalled,
                     const std::function<std::vector<std::uint64_t>(const Reference&)>& choose)
{
    const FrozenClock frozen;
    Compared compared;
    compared.today = RunReference(base, scenario, limits, journalled, false, 0);
    const Reference kernel = RunReference(base, scenario, limits, journalled, false, KernelPages);
    const Reference& today = compared.today;
    REQUIRE(today.mounted == kernel.mounted);
    REQUIRE(today.total == kernel.total);
    REQUIRE(today.writes == kernel.writes);
    REQUIRE(today.flushes == kernel.flushes);
    REQUIRE(today.final_device->call_digest == kernel.final_device->call_digest);
    REQUIRE(SameBytes(*today.final_device, *kernel.final_device));
    const std::size_t block_size = BlockSizeOf(base);
    for (const std::uint64_t cut : choose(today))
    {
        INFO("stopped after write " << cut << " of " << today.total);
        const Crashed left = RunCrashed(base, scenario, cut, limits, false, 0);
        const Crashed right = RunCrashed(base, scenario, cut, limits, false, KernelPages);
        REQUIRE(left.synced == right.synced);
        RequireSameDevices(*left.device, *right.device);
        RequireSameLosses(left, right, cut);
        if (journalled)
        {
            RequireRecoversTo(left, cut, today, block_size);
            RequireRecoversTo(right, cut, kernel, block_size);
        }
        ++compared.cuts;
    }
    return compared;
}

std::vector<std::uint64_t> EveryWrite(const Reference& reference)
{
    return CutsBetween(0, reference.total, reference.total);
}

void MixedOn(std::string_view variant, bool journalled)
{
    INFO(variant);
    const std::unique_ptr<MemoryDevice> base = Prepared(variant);
    const Compared compared =
        CompareRuns(*base, MixedScenario(), std::nullopt, journalled,
                    [](const Reference& reference) { return EveryWrite(reference); });
    REQUIRE(compared.cuts > 200);
}

void RenameOn(std::string_view variant, bool journalled)
{
    INFO(variant);
    const std::unique_ptr<MemoryDevice> base = Fixture(variant, FixtureSetup());
    const Compared compared =
        CompareRuns(*base, RenameScenario(), std::nullopt, journalled,
                    [](const Reference& reference) { return EveryWrite(reference); });
    REQUIRE(compared.cuts > 50);
}

} // namespace
#endif

TEST_CASE("the kernel's cache stack leaves the same crashes as today's: mixed changes on ext3")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    MixedOn("ext3", true);
#endif
}

TEST_CASE("the kernel's cache stack leaves the same crashes as today's: mixed changes on ext4")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    MixedOn("ext4", true);
#endif
}

TEST_CASE(
    "the kernel's cache stack leaves the same crashes as today's: mixed changes on ext4-64bit")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    MixedOn("ext4-64bit", true);
#endif
}

TEST_CASE("the kernel's cache stack leaves the same crashes as today's: mixed changes on ext2")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    MixedOn("1k", false);
#endif
}

TEST_CASE("the kernel's cache stack leaves the same crashes as today's: renames onto names on ext3")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    RenameOn("ext3", true);
#endif
}

TEST_CASE("the kernel's cache stack leaves the same crashes as today's: renames onto names on ext4")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    RenameOn("ext4", true);
#endif
}

TEST_CASE("the kernel's cache stack leaves the same crashes as today's: renames onto names on "
          "ext4-64bit")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    RenameOn("ext4-64bit", true);
#endif
}

TEST_CASE("the kernel's cache stack leaves the same crashes as today's: renames onto names on ext2")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    RenameOn("1k", false);
    RenameOn("ext4-nojournal", false);
#endif
}

TEST_CASE("the kernel's cache stack leaves the same crashes as today's: split changes")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const SplitVolume& volume : SplitVolumes)
    {
        const std::unique_ptr<MemoryDevice> with_big = PreparedForSplit(volume, true);
        const std::unique_ptr<MemoryDevice> without_big = PreparedForSplit(volume, false);
        for (const Split split : {Split::Extend, Split::TruncateToSome, Split::TruncateToNothing,
                                  Split::Remove, Split::Rename})
        {
            INFO(volume.image << ": " << SplitNames[static_cast<std::size_t>(split)]);
            MemoryDevice& base = split == Split::Extend ? *without_big : *with_big;
            const Scenario scenario = {SplitChange(split)};
            const Compared compared =
                CompareRuns(base, scenario, SplitLimits(volume, split), true,
                            [&](const Reference& reference)
                            {
                                return CutsAroundFlushes(reference, reference.mounted,
                                                         reference.writes[1] + 1, 40);
                            });
            REQUIRE(compared.cuts > 0);
        }
    }
#endif
}

TEST_CASE("the kernel's cache stack leaves the same crashes as today's: the log's wrap")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const std::string_view variant : {"ext3", "ext4"})
    {
        INFO(variant);
        const std::unique_ptr<MemoryDevice> base = Prepared(variant);
        const Compared compared = CompareRuns(
            *base, CycleScenario(150), std::nullopt, true,
            [](const Reference& reference)
            {
                const std::size_t checkpoint = FirstStepWhere(
                    reference, [](const JournalActivity& before, const JournalActivity& after)
                    { return after.checkpoints > before.checkpoints; });
                const std::size_t wrap = FirstStepWhere(
                    reference, [](const JournalActivity& before, const JournalActivity& after)
                    { return after.wraps > before.wraps; });
                REQUIRE(checkpoint >= 2);
                REQUIRE(wrap >= 2);
                const std::size_t first = std::min(checkpoint, wrap) - 1;
                const std::size_t last = std::max(checkpoint, wrap) + 1;
                std::vector<std::uint64_t> cuts =
                    CutsBetween(reference.writes[first - 1], reference.writes[last], 100);
                const std::vector<std::uint64_t> unmount =
                    CutsBetween(reference.writes.back(), reference.total, 100);
                cuts.insert(cuts.end(), unmount.begin(), unmount.end());
                return cuts;
            });
        REQUIRE(compared.cuts > 50);
    }
#endif
}
