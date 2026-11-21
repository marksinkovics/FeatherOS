// SPDX-License-Identifier: MIT OR Apache-2.0
#include "tests/crash_helpers.h"

#include "tests/ext2_image.h"
#include "tests/filesystem_helpers.h"
#include "tests/page_cache_support.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <random>
#include <set>

namespace FeatherDisk::Tests::Crash
{

using Ext::Ext2Type;

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

std::string PatternAt(std::uint64_t offset, std::size_t length, unsigned shift)
{
    std::string text(length, '\0');
    for (std::size_t index = 0; index < length; ++index)
    {
        text[index] = static_cast<char>('a' + (offset + index + shift) % 23);
    }
    return text;
}

namespace
{

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

} // namespace

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

// A change that does not fit the journal's limits fails whole and is skipped when asked to.
std::size_t Run(Filesystem& filesystem, const Scenario& scenario,
                const std::function<void(Filesystem&, std::size_t)>& observe, std::size_t* skipped)
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

namespace
{

// What the volume is mounted over: the device as Mountable gives it, or a CachedDevice with its
// own small cache.
class MountLayer
{
public:
    MountLayer(MemoryDevice& device, std::size_t pages) : target(&Mountable(device))
    {
        if (pages > 0)
        {
            REQUIRE(cache.Initialize(source, pages).has_value());
            cached.Attach(&cache, device);
            target = &cached;
        }
    }

    BlockDevice& Device()
    {
        return *target;
    }

private:
    TestPageSource source;
    PageCache cache;
    CachedDevice cached;
    BlockDevice* target;
};

} // namespace

Reference RunReference(MemoryDevice& base, const Scenario& scenario,
                       std::optional<JournalLimits> limits, bool keep_trees, bool skip_no_space,
                       std::size_t page_cache_pages)
{
    Reference reference;
    if (keep_trees)
    {
        reference.trees.push_back(TreeOf(base));
    }
    std::unique_ptr<MemoryDevice> device = CopyOf(base);
    MountLayer layer(*device, page_cache_pages);
    Filesystem* filesystem = Value(Ext2Type().Mount(layer.Device(), {.read_only = false}));
    if (limits)
    {
        Core(*filesystem).LimitJournal(*limits);
    }
    reference.mounted = device->write_count;
    reference.writes.push_back(device->write_count);
    reference.activity.push_back(Core(*filesystem).Activity().value_or(JournalActivity{}));
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
            reference.activity.push_back(Core(live).Activity().value_or(JournalActivity{}));
        },
        skip_no_space ? &skipped : nullptr);
    REQUIRE(synced + skipped == scenario.size());
    reference.skipped = skipped;
    Succeeds(Ext2Type().Unmount(*filesystem));
    reference.total = device->write_count;
    reference.flushes = device->flush_writes;
    CheckClean(*device, "crash-reference");
    reference.final_device = std::move(device);
    return reference;
}

// The scenario on a copy of base whose writes stop after cut, to the unmount.
Crashed RunCrashed(MemoryDevice& base, const Scenario& scenario, std::uint64_t cut,
                   std::optional<JournalLimits> limits, bool skip_no_space,
                   std::size_t page_cache_pages)
{
    Crashed crashed;
    crashed.device = CopyOf(base);
    crashed.device->TrackUnflushed();
    crashed.device->FailWritesAfter(cut);
    MountLayer layer(*crashed.device, page_cache_pages);
    const Result<Filesystem*> mounted = Ext2Type().Mount(layer.Device(), {.read_only = false});
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
        if (!Core(**mounted).Activity().has_value())
        {
            // Without a journal nothing stops the unmount from writing once the device takes
            // writes again, so the stopped volume is kept as a copy and the mount ends on the
            // device.
            std::unique_ptr<MemoryDevice> stopped = CopyOf(*crashed.device);
            stopped->write_count = crashed.device->write_count;
            stopped->flush_writes = crashed.device->flush_writes;
            stopped->call_digest = crashed.device->call_digest;
            crashed.device->FailWritesAfter(UINT64_MAX);
            Succeeds(Ext2Type().Unmount(**mounted));
            crashed.device = std::move(stopped);
            return crashed;
        }
        // The failed unmount leaves the mount to be freed; after an abort it writes nothing.
        const std::uint64_t writes = crashed.device->write_count;
        Succeeds(Ext2Type().Unmount(**mounted));
        REQUIRE(crashed.device->write_count == writes);
    }
    return crashed;
}

// Whether got's block holds what reference does, apart from a suffix that was zeroed (a

namespace
{

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

} // namespace

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

} // namespace FeatherDisk::Tests::Crash
