// SPDX-License-Identifier: MIT OR Apache-2.0
#include "tests/crash_scenarios.h"

#include "ext/journal.h"
#include "fs/ext2.h"
#include "tests/ext2_image.h"
#include "tests/filesystem_helpers.h"
#include "tests/page_cache_support.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <random>
#include <set>
#include <string>

namespace FeatherDisk::Tests::Crash
{

using Ext::Ext2Type;

namespace
{

constexpr std::size_t Extended = 400 << 10;

} // namespace

// About 26 changes: files and directories made, written, overwritten, truncated, moved, linked
// and removed, blocks freed by one change and taken by a later one, and a directory that grows
// past a block. Every file that ever held the 0xA5 fill has "a5" in its name.
Scenario MixedScenario()
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

std::size_t LeafCapacity(std::uint32_t block_size)
{
    return (block_size - 12) / 12;
}

std::uint64_t ReuseLastBlock(std::uint32_t block_size)
{
    return 2 * (LeafCapacity(block_size) - 1) - 6;
}

Change ReuseBuild(std::string path, std::uint32_t block_size)
{
    return [path, block_size](Filesystem& fs) -> Status
    {
        const Result<NodeId> file = fs.Create(fs.Root(), path, 0644);
        const Result<NodeId> decoy = fs.Create(fs.Root(), "decoy.bin", 0644);
        if (!file || !decoy)
        {
            return std::unexpected(!file ? file.error() : decoy.error());
        }
        const std::size_t extents = LeafCapacity(block_size) - 1;
        std::uint64_t logical = 2;
        for (std::size_t index = 0; index < extents; ++index)
        {
            const std::uint64_t offset = logical * block_size;
            if (const Status done = Put("/" + path, offset, PatternAt(offset, block_size))(fs);
                !done)
            {
                return done;
            }
            if (index < 6)
            {
                if (const Status done = Put("/decoy.bin", index * std::uint64_t{block_size},
                                            std::string(block_size, 'D'))(fs);
                    !done)
                {
                    return done;
                }
                ++logical;
            }
            else
            {
                logical += 2;
            }
        }
        return {};
    };
}

Change ReuseSpan(std::string path, std::uint32_t block_size)
{
    return Put("/" + path, 0, ReuseContents(block_size));
}

std::string ReuseContents(std::uint32_t block_size)
{
    return PatternAt(0, (ReuseLastBlock(block_size) + 1) * block_size);
}

Scenario ReuseScenario(std::uint32_t block_size)
{
    Scenario scenario;
    scenario.push_back(
        Together({ReuseBuild("reuse.bin", block_size), ReuseSpan("reuse.bin", block_size)}));
    scenario.push_back(Put("/reuse.bin",
                           (ReuseLastBlock(block_size) + 2) * std::uint64_t{block_size},
                           PatternAt(0, block_size, 7)));
    scenario.push_back(Rm("/reuse.bin"));
    scenario.push_back(Make("/after.bin", PatternAt(0, 120 * std::size_t{block_size})));
    return scenario;
}

// The first change of every group of thirty creates or appends to a file, the next truncates,
// then changes modes, then removes, as the wrap test of the transactions does.
Scenario CycleScenario(std::size_t count)
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
    Filesystem* filesystem = Value(Ext2Type().Mount(Mountable(*device), {.read_only = false}));
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

Scenario RenameScenario()
{
    Scenario scenario;
    for (std::size_t index = 0; index < Replacements.size(); ++index)
    {
        if (index == MadeInScenario)
        {
            scenario.push_back(Make(std::string(Replacements[index].source), "n"));
        }
        scenario.push_back(
            Move(std::string(Replacements[index].source), std::string(Replacements[index].target)));
    }
    return scenario;
}

Scenario FixtureSetup()
{
    Scenario setup;
    setup.push_back(Mkdir("/d"));
    setup.push_back(Make("/d/old.txt", PatternAt(0, 6000)));
    setup.push_back(Make("/src.txt", PatternAt(0, 3000, 5)));
    setup.push_back(Make("/d/x.txt", PatternAt(0, 2000, 9)));
    setup.push_back(Make("/d/y.txt", PatternAt(0, 1500, 13)));
    setup.push_back(Make("/h1", "hard\n"));
    setup.push_back(Hard("/h1", "/h2"));
    setup.push_back(Make("/f.txt", "file\n"));
    setup.push_back(Symlink("/ln", "d/old.txt"));
    setup.push_back(Mkdir("/e2"));
    setup.push_back(Make("/e2/child", "child\n"));
    setup.push_back(Mkdir("/x"));
    setup.push_back(Mkdir("/x/empty"));
    setup.push_back(Make("/big.bin", PatternAt(0, 400 << 10, 3)));
    return setup;
}

std::unique_ptr<MemoryDevice> Fixture(std::string_view variant, const Scenario& setup)
{
    std::unique_ptr<MemoryDevice> device = Prepared(variant);
    Filesystem* filesystem = Value(Ext2Type().Mount(Mountable(*device), {.read_only = false}));
    REQUIRE(Run(*filesystem, setup) == setup.size());
    Succeeds(Ext2Type().Unmount(*filesystem));
    return device;
}

Scenario RandomScenario(std::uint32_t seed, std::size_t count, bool big)
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

} // namespace FeatherDisk::Tests::Crash
