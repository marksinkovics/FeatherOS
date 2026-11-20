// SPDX-License-Identifier: MIT OR Apache-2.0
// Scenarios of changes run on a mounted ext volume, stopped after every write, to compare what a
// crash and its recovery leave with the states the changes committed.
#ifndef FEATHERDISK_TESTS_CRASH_HELPERS_H
#define FEATHERDISK_TESTS_CRASH_HELPERS_H

#include "ext/ext2_filesystem.h"
#include "ext/journal.h"
#include "fs/ext2.h"
#include "fs/filesystem.h"
#include "tests/ext2_image.h"
#include "tests/memory_device.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace FeatherDisk::Tests::Crash
{

using Ext::Ext2Filesystem;
using Ext::JournalActivity;
using Ext::JournalLimits;

Ext2Filesystem& Core(Filesystem& filesystem);
JournalActivity ActivityOf(Filesystem& filesystem);
std::string PatternAt(std::uint64_t offset, std::size_t length, unsigned shift = 0);

using Change = std::function<Status(Filesystem&)>;
using Scenario = std::vector<Change>;

Change Mkdir(std::string path);
Change Make(std::string path, std::string text);
Change Put(std::string path, std::uint64_t offset, std::string text);
Change Cut(std::string path, std::uint64_t size);
Change Rm(std::string path);
Change Rmdir(std::string path);
Change Move(std::string from, std::string to);
Change Hard(std::string existing, std::string path);
Change Symlink(std::string path, std::string target);
Change Mode(std::string path, std::uint32_t mode);
Change Together(std::vector<Change> parts);

// Every file that ever held the 0xA5 fill has "a5" in its name.
constexpr std::string_view A5Name = "a5";

// A change that does not fit the journal's limits fails whole and is skipped when asked to.
std::size_t Run(Filesystem& filesystem, const Scenario& scenario,
                const std::function<void(Filesystem&, std::size_t)>& observe = {},
                std::size_t* skipped = nullptr);

// What an uninterrupted run did: the tree after each successful Sync (trees[0] is the volume as
// it was) and the device's write count at the same points.
struct Reference
{
    std::vector<Tree> trees;
    std::vector<std::uint64_t> writes;
    std::vector<JournalActivity> activity;
    // The write count at each Flush, which is where a commit or a checkpoint reached the device.
    std::vector<std::uint64_t> flushes;
    std::uint64_t mounted = 0;
    std::uint64_t total = 0;
    std::size_t skipped = 0;
    // The volume after the run and its unmount.
    std::unique_ptr<MemoryDevice> final_device;
};

Reference RunReference(MemoryDevice& base, const Scenario& scenario,
                       std::optional<JournalLimits> limits, bool keep_trees = true,
                       bool skip_no_space = false);

struct Crashed
{
    std::unique_ptr<MemoryDevice> device;
    std::size_t synced = 0;
};

// The scenario on a copy of base whose writes stop after cut, to the unmount.
Crashed RunCrashed(MemoryDevice& base, const Scenario& scenario, std::uint64_t cut,
                   std::optional<JournalLimits> limits, bool skip_no_space = false);

// The tree is that of `synced` changes (whose blocks may hold bytes of the next change's
// in-place writes) or of one more, whole.
void RequireCommittedState(const Tree& got, const std::vector<Tree>& trees, std::size_t synced,
                           std::size_t block_size);
void RequireNoStaleFill(const Tree& tree);
// The volume as a drive that lost a random part of its unflushed writes would leave it: the
// journal replays to a committed state whichever writes did not reach it.
void RequireLossyRecovery(const Crashed& crashed, std::uint64_t cut, const std::vector<Tree>& trees,
                          std::size_t block_size);
std::uint32_t BlockSizeOf(MemoryDevice& device);
std::unique_ptr<MemoryDevice> Prepared(std::string_view variant);

// At most budget of the cuts from first to last, all of them when there are fewer.
std::vector<std::uint64_t> CutsBetween(std::uint64_t first, std::uint64_t last, std::size_t budget);
// Every cut within a few writes of a flush (the writes of a commit's last blocks and the ones
// right after it), and an even sample of the rest.
std::vector<std::uint64_t> CutsAroundFlushes(const Reference& reference, std::uint64_t first,
                                             std::uint64_t last, std::size_t sample);

} // namespace FeatherDisk::Tests::Crash

#endif
