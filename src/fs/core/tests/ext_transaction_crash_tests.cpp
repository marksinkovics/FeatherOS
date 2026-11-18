// SPDX-License-Identifier: MIT OR Apache-2.0
// What a crash leaves on ext3 and ext4 volumes written through the journal: runs stopped after
// every write (or every write of the interesting stretches), recovered by our own writable mount
// and by e2fsck -fy, must hold the tree of a committed state and nothing of a stale block.
#include <catch2/catch_test_macros.hpp>

#include "ext/ext2_filesystem.h"
#include "fs/ext2.h"
#include "tests/ext2_image.h"
#include "tests/filesystem_helpers.h"
#include "tests/memory_device.h"

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
using Ext::Ext2Filesystem;
using Ext::Ext2Type;
using Ext::JournalActivity;
using Ext::JournalLimits;

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

std::string PatternAt(std::uint64_t offset, std::size_t length, unsigned shift = 0)
{
    std::string text(length, '\0');
    for (std::size_t index = 0; index < length; ++index)
    {
        text[index] = static_cast<char>('a' + (offset + index + shift) % 23);
    }
    return text;
}

using Change = std::function<Status(Filesystem&)>;
using Scenario = std::vector<Change>;

template <typename T> Status StatusOf(const Result<T>& result)
{
    return result ? Status{} : std::unexpected(result.error());
}

Result<NodeId> DirectoryOf(Filesystem& filesystem, std::string_view path)
{
    const std::size_t separator = path.rfind('/');
    return separator == std::string_view::npos ? Result<NodeId>(filesystem.Root())
                                               : Walk(filesystem, path.substr(0, separator));
}

std::string_view LeafOf(std::string_view path)
{
    const std::size_t separator = path.rfind('/');
    return separator == std::string_view::npos ? path : path.substr(separator + 1);
}

Status WriteText(Filesystem& filesystem, NodeId file, std::uint64_t offset, const std::string& text)
{
    const Result<std::size_t> written = filesystem.Write(file, offset, BytesOfText(text));
    if (!written)
    {
        return std::unexpected(written.error());
    }
    return *written == text.size() ? Status{} : std::unexpected(Error::NoSpace);
}

Change Mkdir(std::string path)
{
    return [path](Filesystem& fs) -> Status
    {
        const Result<NodeId> parent = DirectoryOf(fs, path);
        if (!parent)
        {
            return std::unexpected(parent.error());
        }
        return StatusOf(fs.MakeDirectory(*parent, LeafOf(path), 0755));
    };
}

Change Make(std::string path, std::string text)
{
    return [path, text](Filesystem& fs) -> Status
    {
        const Result<NodeId> parent = DirectoryOf(fs, path);
        if (!parent)
        {
            return std::unexpected(parent.error());
        }
        const Result<NodeId> file = fs.Create(*parent, LeafOf(path), 0644);
        if (!file)
        {
            return std::unexpected(file.error());
        }
        return text.empty() ? Status{} : WriteText(fs, *file, 0, text);
    };
}

Change Put(std::string path, std::uint64_t offset, std::string text)
{
    return [path, offset, text](Filesystem& fs) -> Status
    {
        const Result<NodeId> file = Walk(fs, path);
        return file ? WriteText(fs, *file, offset, text) : std::unexpected(file.error());
    };
}

Change Cut(std::string path, std::uint64_t size)
{
    return [path, size](Filesystem& fs) -> Status
    {
        const Result<NodeId> file = Walk(fs, path);
        return file ? fs.Truncate(*file, size) : std::unexpected(file.error());
    };
}

Change Rm(std::string path)
{
    return [path](Filesystem& fs) -> Status
    {
        const Result<NodeId> parent = DirectoryOf(fs, path);
        return parent ? fs.Remove(*parent, LeafOf(path)) : std::unexpected(parent.error());
    };
}

Change Rmdir(std::string path)
{
    return [path](Filesystem& fs) -> Status
    {
        const Result<NodeId> parent = DirectoryOf(fs, path);
        return parent ? fs.RemoveDirectory(*parent, LeafOf(path)) : std::unexpected(parent.error());
    };
}

Change Move(std::string from, std::string to)
{
    return [from, to](Filesystem& fs) -> Status
    {
        const Result<NodeId> source = DirectoryOf(fs, from);
        const Result<NodeId> target = DirectoryOf(fs, to);
        if (!source || !target)
        {
            return std::unexpected(!source ? source.error() : target.error());
        }
        return fs.Rename(*source, LeafOf(from), *target, LeafOf(to));
    };
}

Change Hard(std::string existing, std::string path)
{
    return [existing, path](Filesystem& fs) -> Status
    {
        const Result<NodeId> node = Walk(fs, existing);
        const Result<NodeId> parent = DirectoryOf(fs, path);
        if (!node || !parent)
        {
            return std::unexpected(!node ? node.error() : parent.error());
        }
        return fs.Link(*node, *parent, LeafOf(path));
    };
}

Change Symlink(std::string path, std::string target)
{
    return [path, target](Filesystem& fs) -> Status
    {
        const Result<NodeId> parent = DirectoryOf(fs, path);
        return parent ? StatusOf(fs.SymbolicLink(*parent, LeafOf(path), target))
                      : std::unexpected(parent.error());
    };
}

Change Mode(std::string path, std::uint32_t mode)
{
    return [path, mode](Filesystem& fs) -> Status
    {
        const Result<NodeId> node = Walk(fs, path);
        AttributeChanges changes{};
        changes.mode = mode;
        return node ? fs.SetAttributes(*node, changes) : std::unexpected(node.error());
    };
}

Change Together(std::vector<Change> parts)
{
    return [parts](Filesystem& fs) -> Status
    {
        for (const Change& part : parts)
        {
            if (const Status done = part(fs); !done)
            {
                return done;
            }
        }
        return {};
    };
}

constexpr std::string_view A5Name = "a5";

// About 26 changes: files and directories made, written, overwritten, truncated, moved, linked
// and removed, blocks freed by one change and taken by a later one, and a directory that grows
// past a block. Every file that ever held the 0xA5 fill has "a5" in its name.
Scenario MakeScenario()
{
    Scenario scenario;
    const auto add = [&](Change change) { scenario.push_back(std::move(change)); };
    add(Mkdir("/d"));
    add(Mkdir("/d/e"));
    add(Make("/a5.bin", std::string(6000, static_cast<char>(0xA5))));
    add(Make("/d/a.txt", PatternAt(0, 3000)));
    add(Make("/big.bin", PatternAt(0, 20000, 4)));
    add(Symlink("/fast", "d/a.txt"));
    add(Symlink("/slow", std::string(100, 'x') + "/long/slow/link/target"));
    add(Hard("/d/a.txt", "/d/hard"));
    add(Put("/d/a.txt", 100, PatternAt(0, 1500, 9)));
    add(Move("/d/a.txt", "/d/e/moved.txt"));
    add(Rm("/a5.bin"));
    add(Make("/reuse.bin", PatternAt(0, 6000, 11)));
    add(Cut("/big.bin", 5000));
    add(Cut("/big.bin", 30000));
    add(Mode("/reuse.bin", 0600));
    add(Rm("/slow"));
    add(Move("/reuse.bin", "/d/hard"));
    {
        std::vector<Change> names;
        for (int number = 0; number < 40; ++number)
        {
            names.push_back(Make("/d/e/" + NumberedName("file-with-a-long-name", number, 36),
                                 number % 3 == 0 ? PatternAt(number, 700 + number * 40) : ""));
        }
        add(Together(std::move(names)));
    }
    add(Mkdir("/d/e/sub"));
    add(Rmdir("/d/e/sub"));
    add(Make("/d/e/empty", ""));
    add(Put("/d/e/empty", 0, PatternAt(0, 8000, 2)));
    {
        std::vector<Change> names;
        for (int number = 0; number < 40; number += 2)
        {
            names.push_back(Rm("/d/e/" + NumberedName("file-with-a-long-name", number, 36)));
        }
        add(Together(std::move(names)));
    }
    add(Cut("/big.bin", 0));
    add(Rm("/big.bin"));
    {
        std::vector<Change> names;
        for (int number = 1; number < 40; number += 2)
        {
            names.push_back(Rm("/d/e/" + NumberedName("file-with-a-long-name", number, 36)));
        }
        names.push_back(Rm("/d/e/moved.txt"));
        names.push_back(Rm("/d/e/empty"));
        names.push_back(Rmdir("/d/e"));
        add(Together(std::move(names)));
    }
    return scenario;
}

// A change that does not fit the journal's limits fails whole and is skipped when asked to.
std::size_t Run(Filesystem& filesystem, const Scenario& scenario,
                const std::function<void(Filesystem&, std::size_t)>& observe = {},
                std::size_t* skipped = nullptr)
{
    std::size_t synced = 0;
    for (const Change& change : scenario)
    {
        const Status done = change(filesystem);
        if (!done && skipped != nullptr && done.error() == Error::NoSpace)
        {
            ++*skipped;
            continue;
        }
        if (!done || !filesystem.Sync())
        {
            break;
        }
        ++synced;
        if (observe)
        {
            observe(filesystem, synced);
        }
    }
    return synced;
}

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
};

Reference RunReference(MemoryDevice& base, const Scenario& scenario,
                       std::optional<JournalLimits> limits, bool keep_trees = true,
                       bool skip_no_space = false)
{
    Reference reference;
    if (keep_trees)
    {
        reference.trees.push_back(TreeOf(base));
    }
    const std::unique_ptr<MemoryDevice> device = CopyOf(base);
    Filesystem* filesystem = Value(Ext2Type().Mount(*device, {.read_only = false}));
    if (limits)
    {
        Core(*filesystem).LimitJournal(*limits);
    }
    reference.mounted = device->write_count;
    reference.writes.push_back(device->write_count);
    reference.activity.push_back(ActivityOf(*filesystem));
    std::size_t skipped = 0;
    const std::size_t synced = Run(
        *filesystem, scenario,
        [&](Filesystem& live, std::size_t)
        {
            if (keep_trees)
            {
                reference.trees.push_back(TreeOf(live));
            }
            reference.writes.push_back(device->write_count);
            reference.activity.push_back(ActivityOf(live));
        },
        skip_no_space ? &skipped : nullptr);
    REQUIRE(synced + skipped == scenario.size());
    reference.skipped = skipped;
    Succeeds(Ext2Type().Unmount(*filesystem));
    reference.total = device->write_count;
    reference.flushes = device->flush_writes;
    CheckClean(*device, "crash-reference");
    return reference;
}

struct Crashed
{
    std::unique_ptr<MemoryDevice> device;
    std::size_t synced = 0;
};

// The scenario on a copy of base whose writes stop after cut, to the unmount.
Crashed RunCrashed(MemoryDevice& base, const Scenario& scenario, std::uint64_t cut,
                   std::optional<JournalLimits> limits, bool skip_no_space = false)
{
    Crashed crashed;
    crashed.device = CopyOf(base);
    crashed.device->TrackUnflushed();
    crashed.device->FailWritesAfter(cut);
    const Result<Filesystem*> mounted = Ext2Type().Mount(*crashed.device, {.read_only = false});
    if (!mounted)
    {
        return crashed;
    }
    if (limits)
    {
        Core(**mounted).LimitJournal(*limits);
    }
    std::size_t skipped = 0;
    crashed.synced = Run(**mounted, scenario, {}, skip_no_space ? &skipped : nullptr);
    if (!Ext2Type().Unmount(**mounted))
    {
        // The failed unmount leaves the mount to be freed; after an abort it writes nothing.
        const std::uint64_t writes = crashed.device->write_count;
        Succeeds(Ext2Type().Unmount(**mounted));
        REQUIRE(crashed.device->write_count == writes);
    }
    return crashed;
}

// Whether got's block holds what reference does, apart from a suffix that was zeroed (a
// truncation's last block) and bytes the reference does not have.
bool BlockLooksLike(const std::string& got, const std::string& reference)
{
    std::size_t at = 0;
    while (at < got.size() && at < reference.size() && got[at] == reference[at])
    {
        ++at;
    }
    return std::all_of(got.begin() + static_cast<std::ptrdiff_t>(at), got.end(),
                       [](char byte) { return byte == '\0'; });
}

bool ContentsOk(const Tree& got, const Tree& exact, const Tree* other, std::size_t block_size,
                std::string& why)
{
    for (const auto& [path, entry] : got)
    {
        if (entry.type != NodeType::File || entry.contents == exact.at(path).contents)
        {
            continue;
        }
        if (other == nullptr || !other->contains(path))
        {
            why = path + ": contents differ";
            return false;
        }
        const std::string& before = exact.at(path).contents;
        const std::string& after = other->at(path).contents;
        for (std::size_t at = 0; at < entry.contents.size(); at += block_size)
        {
            const std::string block = entry.contents.substr(at, block_size);
            const auto part = [&](const std::string& text)
            { return at < text.size() ? text.substr(at, block_size) : std::string(); };
            if (!BlockLooksLike(block, part(before)) && !BlockLooksLike(block, part(after)))
            {
                why = path + ": block at " + std::to_string(at) + " is neither old nor new";
                return false;
            }
        }
    }
    return true;
}

// The tree is that of `synced` changes (whose blocks may hold bytes of the next change's
// in-place writes) or of one more, whole.
void RequireCommittedState(const Tree& got, const std::vector<Tree>& trees, std::size_t synced,
                           std::size_t block_size)
{
    const std::size_t next = std::min(synced + 1, trees.size() - 1);
    std::string why;
    if (TreeDifference(got, trees[synced], false).empty() &&
        ContentsOk(got, trees[synced], &trees[next], block_size, why))
    {
        return;
    }
    const std::string first = why.empty() ? TreeDifference(got, trees[synced], false) : why;
    if (TreeDifference(got, trees[next], false).empty() &&
        ContentsOk(got, trees[next], nullptr, block_size, why))
    {
        return;
    }
    INFO("synced " << synced << ": " << first
                   << "; next: " << (why.empty() ? TreeDifference(got, trees[next], false) : why));
    FAIL("not a committed state");
}

void RequireNoStaleFill(const Tree& tree)
{
    for (const auto& [path, entry] : tree)
    {
        if (path.find(A5Name) == std::string::npos)
        {
            INFO(path);
            REQUIRE(entry.contents.find(static_cast<char>(0xA5)) == std::string::npos);
        }
    }
}

// The volume as a drive that lost a random part of its unflushed writes would leave it: the
// journal replays to a committed state whichever writes did not reach it.
void RequireLossyRecovery(const Crashed& crashed, std::uint64_t cut, const std::vector<Tree>& trees,
                          std::size_t block_size)
{
    if (crashed.device->UnflushedCount() < 2)
    {
        return;
    }
    std::mt19937 random(static_cast<std::uint32_t>(cut) * 2654435761U);
    const std::unique_ptr<MemoryDevice> lossy =
        crashed.device->CopyKeeping([&](std::size_t) { return random() % 2 == 0; });
    ReplayByMounting(*lossy);
    CheckClean(*lossy, "crash-lossy");
    const Tree tree = TreeOf(*lossy);
    INFO("with " << crashed.device->UnflushedCount() << " unflushed writes, some lost");
    RequireCommittedState(tree, trees, crashed.synced, block_size);
    RequireNoStaleFill(tree);
}

std::uint32_t BlockSizeOf(MemoryDevice& device)
{
    return 1024U << device.Bytes()[1024 + 24];
}

} // namespace

namespace
{

std::unique_ptr<MemoryDevice> Prepared(std::string_view variant)
{
    if (variant == "ext4-blockmap-v2")
    {
        std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage("ext4-blockmap"));
        REQUIRE(ScriptWithDebugfs(*device, "crash-v2", "jo -c -v 2\njc\n", {}).status == 0);
        return device;
    }
    return DeviceFrom(LoadExtImage(variant));
}

} // namespace

TEST_CASE("a journalled volume stopped after any write recovers to a committed state")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    // The two default layouts are cut at every write; the others, which differ in the log's
    // format and checksums, at every third.
    struct Layout
    {
        std::string_view variant;
        std::uint64_t stride;
        std::uint64_t first;
    };
    const Scenario scenario = MakeScenario();
    for (const Layout& layout :
         {Layout{"ext3", 1, 0}, Layout{"ext4", 1, 0}, Layout{"ext4-blockmap-v2", 3, 1},
          Layout{"ext4-journal-gdtcsum", 3, 2}, Layout{"ext4-64bit", 3, 0}})
    {
        INFO(layout.variant);
        const std::unique_ptr<MemoryDevice> base = Prepared(layout.variant);
        const Reference reference = RunReference(*base, scenario, std::nullopt);
        const std::size_t block_size = BlockSizeOf(*base);
        REQUIRE(reference.total > 200);
        for (std::uint64_t cut = layout.first; cut < reference.total; cut += layout.stride)
        {
            INFO("stopped after write " << cut << " of " << reference.total);
            const Crashed crashed = RunCrashed(*base, scenario, cut, std::nullopt);
            const Replayed replayed = ReplayBothWays(*crashed.device, "crash-every-write");
            RequireCommittedState(replayed.tree, reference.trees, crashed.synced, block_size);
            RequireNoStaleFill(replayed.tree);
            RequireLossyRecovery(crashed, cut, reference.trees, block_size);
        }
    }
#endif
}

namespace
{

// The first change of every group of thirty creates or appends to a file, the next truncates,
// then changes modes, then removes, as the wrap test of the transactions does.
Scenario MakeCycle(std::size_t count)
{
    Scenario scenario;
    std::map<std::string, std::uint64_t> sizes;
    for (std::size_t step = 0; step < count; ++step)
    {
        const std::string name = "/f" + std::to_string(step % 30);
        switch ((step / 30) % 4)
        {
        case 0:
        {
            const std::string text = PatternAt(step, 150 + (step * 37) % 900);
            if (sizes.contains(name))
            {
                scenario.push_back(Put(name, sizes[name], text));
                sizes[name] += text.size();
            }
            else
            {
                scenario.push_back(Make(name, text));
                sizes[name] = text.size();
            }
            break;
        }
        case 1:
            sizes[name] /= 2;
            scenario.push_back(Cut(name, sizes[name]));
            break;
        case 2:
            scenario.push_back(Mode(name, step % 2 == 0 ? 0600 : 0644));
            break;
        default:
            sizes.erase(name);
            scenario.push_back(Rm(name));
            break;
        }
    }
    return scenario;
}

// At most budget of the cuts from first to last, all of them when there are fewer.
std::vector<std::uint64_t> CutsBetween(std::uint64_t first, std::uint64_t last, std::size_t budget)
{
    std::vector<std::uint64_t> cuts;
    const std::uint64_t count = last - first;
    const std::uint64_t stride = std::max<std::uint64_t>(1, (count + budget - 1) / budget);
    for (std::uint64_t cut = first; cut < last; cut += stride)
    {
        cuts.push_back(cut);
    }
    return cuts;
}

// Every cut within a few writes of a flush (the writes of a commit's last blocks and the ones
// right after it), and an even sample of the rest.
std::vector<std::uint64_t> CutsAroundFlushes(const Reference& reference, std::uint64_t first,
                                             std::uint64_t last, std::size_t sample)
{
    std::set<std::uint64_t> cuts;
    for (const std::uint64_t mark : reference.flushes)
    {
        for (std::uint64_t cut = mark > 3 ? mark - 3 : 0; cut <= mark + 2; ++cut)
        {
            if (cut >= first && cut < last)
            {
                cuts.insert(cut);
            }
        }
    }
    for (const std::uint64_t cut : CutsBetween(first, last, sample))
    {
        cuts.insert(cut);
    }
    return {cuts.begin(), cuts.end()};
}

std::size_t
FirstStepWhere(const Reference& reference,
               const std::function<bool(const JournalActivity&, const JournalActivity&)>& changed)
{
    for (std::size_t step = 1; step < reference.activity.size(); ++step)
    {
        if (changed(reference.activity[step - 1], reference.activity[step]))
        {
            return step;
        }
    }
    return 0;
}

} // namespace

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
        const Scenario scenario = MakeCycle(150);
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

namespace
{

struct SplitVolume
{
    std::string_view image;
    std::uint32_t hard;
    std::size_t big;
    // big.bin written a block at a time between the blocks of a filler that is removed again,
    // so on extents its mapping is a tree of single-block extents.
    bool fragmented;
};

constexpr SplitVolume SplitVolumes[] = {
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

constexpr std::string_view SplitNames[] = {"extend", "truncate to 1000", "truncate to 0", "remove",
                                           "rename onto"};

constexpr std::size_t Extended = 400 << 10;

std::unique_ptr<MemoryDevice> PreparedForSplit(const SplitVolume& volume, bool with_big)
{
    std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(volume.image));
    Scenario setup;
    if (volume.fragmented)
    {
        setup.push_back(Make("/big.bin", ""));
        setup.push_back(Make("/filler.bin", ""));
        for (std::size_t block = 0; block < volume.big / 1024; ++block)
        {
            setup.push_back(Put("/big.bin", block * 1024, PatternAt(block * 1024, 1024)));
            setup.push_back(Put("/filler.bin", block * 1024, PatternAt(block * 1024, 1024, 7)));
        }
        setup.push_back(Rm("/filler.bin"));
    }
    else if (with_big)
    {
        setup.push_back(Make("/big.bin", PatternAt(0, volume.big)));
    }
    setup.push_back(Make("/src.txt", "src"));
    setup.push_back(Make("/w.bin", PatternAt(0, 3000, 5)));
    Filesystem* filesystem = Value(Ext2Type().Mount(*device, {.read_only = false}));
    REQUIRE(Run(*filesystem, setup) == setup.size());
    Succeeds(Ext2Type().Unmount(*filesystem));
    return device;
}

Change SplitChange(Split split)
{
    switch (split)
    {
    case Split::Extend:
        return Put("/w.bin", 0, PatternAt(0, Extended, 3));
    case Split::TruncateToSome:
        return Cut("/big.bin", 1000);
    case Split::TruncateToNothing:
        return Cut("/big.bin", 0);
    case Split::Remove:
        return Rm("/big.bin");
    case Split::Rename:
        return Move("/src.txt", "/big.bin");
    }
    return {};
}

JournalLimits SplitLimits(const SplitVolume& volume, Split split)
{
    return {
        .soft = 3, .hard = split == Split::Extend ? volume.hard + 2 : volume.hard, .committed = 32};
}

} // namespace

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
            for (const std::uint64_t cut : cuts)
            {
                INFO("stopped after write " << cut << " of " << reference.writes[1]);
                const Crashed crashed = RunCrashed(*base, scenario, cut, limits);
                const Replayed replayed = ReplayBothWays(*crashed.device, "crash-split");
                pending += replayed.orphans;
                RequireCommittedState(replayed.tree, reference.trees, 0, block_size);
            }
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
        Filesystem* filesystem = Value(Ext2Type().Mount(*probe, {.read_only = false}));
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
        Filesystem* filesystem = Value(Ext2Type().Mount(*whole, {.read_only = false}));
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
            const Result<Filesystem*> mounted = Ext2Type().Mount(*stopped, {.read_only = false});
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

Scenario MakeRandomScenario(std::uint32_t seed, std::size_t count, bool big)
{
    std::mt19937 random(seed);
    Scenario scenario;
    std::map<std::string, std::uint64_t> files;
    std::set<std::string> links;
    const auto add = [&](Change change) { scenario.push_back(std::move(change)); };
    add(Mkdir("/d0"));
    add(Mkdir("/d1"));
    const std::string directories[] = {"/", "/d0/", "/d1/"};
    const auto pick = [&]() -> std::string
    {
        auto it = files.begin();
        std::advance(it, random() % files.size());
        return it->first;
    };
    for (std::size_t step = 0; step < count; ++step)
    {
        const std::uint32_t choice = random() % 100;
        const std::string name = directories[random() % 3] + "f" + std::to_string(random() % 8);
        if (choice < 35 || files.empty())
        {
            const std::size_t length = 1 + random() % (big && random() % 40 == 0 ? 200000 : 12000);
            if (!files.contains(name))
            {
                add(Make(name, ""));
                add(Put(name, 0, PatternAt(step, length, static_cast<unsigned>(step))));
                files[name] = length;
            }
            else
            {
                const std::uint64_t offset = random() % (files[name] + 1);
                add(Put(name, offset, PatternAt(step, length, static_cast<unsigned>(step))));
                files[name] = std::max<std::uint64_t>(files[name], offset + length);
            }
        }
        else if (choice < 47)
        {
            const std::string target = pick();
            files[target] = random() % (files[target] + 3000);
            add(Cut(target, files[target]));
        }
        else if (choice < 59)
        {
            const std::string target = pick();
            files.erase(target);
            add(Rm(target));
        }
        else if (choice < 69)
        {
            const std::string source = pick();
            if (name != source && !links.contains(name))
            {
                files[name] = files[source];
                files.erase(source);
                add(Move(source, name));
            }
        }
        else if (choice < 79)
        {
            const std::string link = "/s" + std::to_string(random() % 4);
            if (links.contains(link))
            {
                links.erase(link);
                add(Rm(link));
            }
            else
            {
                links.insert(link);
                add(Symlink(link, std::string(random() % 90 + 5, 'l') + std::to_string(step)));
            }
        }
        else if (choice < 87)
        {
            const std::string link = "/h" + std::to_string(random() % 4);
            if (files.contains(link))
            {
                files.erase(link);
                add(Rm(link));
            }
            else
            {
                const std::string source = pick();
                if (!links.contains(link))
                {
                    files[link] = files[source];
                    add(Hard(source, link));
                }
            }
        }
        else if (choice < 93)
        {
            add(Mode(pick(), random() % 2 == 0 ? 0600 : 0644));
        }
        else
        {
            add(Make("/d1/extra" + std::to_string(step), ""));
        }
    }
    return scenario;
}

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
    Filesystem* filesystem = Value(Ext2Type().Mount(*device, {.read_only = false}));
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
            const Scenario scenario = MakeRandomScenario(seed * 7919, 300, limited);
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
