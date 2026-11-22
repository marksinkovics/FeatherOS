// SPDX-License-Identifier: MIT OR Apache-2.0
// The change scenarios the crash tests share.
#ifndef FEATHERDISK_TESTS_CRASH_SCENARIOS_H
#define FEATHERDISK_TESTS_CRASH_SCENARIOS_H

#include "tests/crash_helpers.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace FeatherDisk::Tests::Crash
{

// About 26 changes: files and directories made, written, overwritten, truncated, moved, linked
// and removed, blocks freed by one change and taken by a later one, and a directory that grows
// past a block.
Scenario MixedScenario();
// The first change of every group of thirty creates or appends to a file, the next truncates,
// then changes modes, then removes, as the wrap test of the transactions does.
Scenario CycleScenario(std::size_t count);
// Random changes of every kind from a seed; big now and then writes 200000 bytes.
Scenario RandomScenario(std::uint32_t seed, std::size_t count, bool big);

struct SplitVolume
{
    std::string_view image;
    std::uint32_t hard;
    std::size_t big;
    // big.bin written a block at a time between the blocks of a filler that is removed again,
    // so on extents its mapping is a tree of single-block extents.
    bool fragmented;
};

inline constexpr SplitVolume SplitVolumes[] = {
    {"ext3-groups", 5, 3 << 20, false},
    {"ext4", 6, 400 << 10, true},
};

enum class Split
{
    Extend,
    TruncateToSome,
    TruncateToNothing,
    Remove,
    Rename,
};

inline constexpr std::string_view SplitNames[] = {"extend", "truncate to 1000", "truncate to 0",
                                                  "remove", "rename onto"};

std::unique_ptr<MemoryDevice> PreparedForSplit(const SplitVolume& volume, bool with_big);
Change SplitChange(Split split);
JournalLimits SplitLimits(const SplitVolume& volume, Split split);

struct Replacement
{
    std::string_view source;
    std::string_view target;
};

inline constexpr std::array<Replacement, 6> Replacements = {{
    {"/src.txt", "/d/old.txt"},
    {"/d/x.txt", "/d/y.txt"},
    {"/f.txt", "/ln"},
    {"/e2", "/x/empty"},
    {"/h1", "/h2"},
    {"/n.txt", "/big.bin"},
}};

inline constexpr std::size_t HardLinkCase = 4;
inline constexpr std::size_t MadeInScenario = 5;

Scenario RenameScenario();
Scenario FixtureSetup();
std::unique_ptr<MemoryDevice> Fixture(std::string_view variant, const Scenario& setup);

} // namespace FeatherDisk::Tests::Crash

#endif
