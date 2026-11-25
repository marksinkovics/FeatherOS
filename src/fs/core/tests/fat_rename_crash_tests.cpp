// SPDX-License-Identifier: MIT OR Apache-2.0
// FAT16 renames stopped after every write. The source survives every cut and every error, a failed
// rename leaves the target as it was, and no cluster ends up with two owners but the source's chain
// for the one moment a rename needs two names over it.
#include <catch2/catch_test_macros.hpp>

#include "fat/names.h"
#include "fs/fat.h"
#include "tests/fat_image.h"
#include "tests/filesystem_helpers.h"
#include "tests/memory_device.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace
{

using namespace FeatherDisk;
using namespace FeatherDisk::Tests;
using Fat::Fat16Type;

using Prepare = std::function<void(Filesystem&)>;

std::unique_ptr<MemoryDevice> Copy(MemoryDevice& device)
{
    auto copy = std::make_unique<MemoryDevice>(device.SectorSize(), device.SectorCount());
    std::memcpy(copy->Bytes().data(), device.Bytes().data(), device.Bytes().size());
    return copy;
}

std::unique_ptr<MemoryDevice> Prepared(const Prepare& prepare, Fat16Layout layout = {})
{
    Fat16Image image(layout);
    {
        MountedFilesystem fat(Fat16Type(), image.Device());
        prepare(*fat);
    }
    return Copy(image.Device());
}

Status RenameTo(Filesystem& fat, const std::string& from, const std::string& to)
{
    const auto [from_directory, from_name] = Parent(fat, from);
    const auto [to_directory, to_name] = Parent(fat, to);
    return fat.Rename(from_directory, from_name, to_directory, to_name);
}

std::string Lower(std::string text)
{
    for (char& letter : text)
    {
        letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
    }
    return text;
}

bool SameSpelling(const std::string& left, const std::string& right)
{
    return Lower(left) == Lower(right);
}

std::string DirectoryOf(const std::string& path)
{
    const std::size_t separator = path.rfind('/');
    return separator == std::string::npos ? std::string{} : path.substr(0, separator + 1);
}

struct Expect
{
    std::string old_path;
    std::string new_path;
    bool replaces = false;
    // The source's short entry never changes its place.
    bool in_place = false;
    // Growing a directory allocates a cluster before it links it, so a stop between the two
    // leaks one, as for any other change.
    bool may_leak = false;
    // Every name the source or the target may show under at a cut.
    std::vector<std::string> names;
    FatEntryInfo source;
    FatEntryInfo target;
    std::string source_data;
    std::string target_data;
};

Expect Describe(MemoryDevice& base, const std::string& old_path, const std::string& new_path,
                bool replaces, bool in_place = false)
{
    Expect expect;
    expect.old_path = old_path;
    expect.new_path = new_path;
    expect.replaces = replaces;
    expect.in_place = in_place;
    const FatScan scan = ScanFat16(base);
    const FatEntryInfo* source = scan.Find(old_path);
    REQUIRE(source != nullptr);
    expect.source = *source;
    expect.names = {old_path, new_path, source->path, DirectoryOf(old_path) + source->short_name};
    if (!source->is_directory)
    {
        expect.source_data = FatContents(base, *source);
    }
    if (replaces)
    {
        const FatEntryInfo* target = scan.Find(new_path);
        REQUIRE(target != nullptr);
        expect.target = *target;
        expect.names.push_back(target->path);
        expect.names.push_back(DirectoryOf(new_path) + target->short_name);
        if (!target->is_directory)
        {
            expect.target_data = FatContents(base, *target);
        }
    }
    return expect;
}

bool Allowed(const Expect& expect, const std::string& path)
{
    return std::any_of(expect.names.begin(), expect.names.end(),
                       [&](const std::string& name) { return SameSpelling(name, path); });
}

bool Contains(const std::vector<std::uint32_t>& chain, std::uint32_t cluster)
{
    return std::find(chain.begin(), chain.end(), cluster) != chain.end();
}

void CheckState(MemoryDevice& device, const Expect& expect, bool completed)
{
    const FatScan scan = ScanFat16(device);
    CHECK(scan.freed_but_owned.empty());

    const std::vector<const FatEntryInfo*> sources = scan.Referencing(expect.source.first_cluster);
    REQUIRE_FALSE(sources.empty());
    REQUIRE(sources.size() <= 2);
    for (const FatEntryInfo* source : sources)
    {
        INFO("source entry " << source->path);
        REQUIRE(Allowed(expect, source->path));
        REQUIRE(source->size == expect.source.size);
        REQUIRE(source->chain == expect.source.chain);
        if (!source->is_directory)
        {
            REQUIRE(FatContents(device, *source) == expect.source_data);
        }
        if (expect.in_place)
        {
            REQUIRE(source->slot == expect.source.slot);
            REQUIRE(source->directory == expect.source.directory);
        }
    }
    if (expect.in_place)
    {
        REQUIRE(sources.size() == 1);
    }
    for (const auto& [cluster, owners] : scan.owners)
    {
        INFO("cluster " << cluster);
        REQUIRE(owners.size() <= (Contains(expect.source.chain, cluster) ? 2U : 1U));
    }

    std::vector<const FatEntryInfo*> targets;
    if (expect.replaces)
    {
        targets = scan.Referencing(expect.target.first_cluster);
        REQUIRE(targets.size() <= 1);
        for (const FatEntryInfo* target : targets)
        {
            INFO("target entry " << target->path);
            REQUIRE(Allowed(expect, target->path));
            REQUIRE(target->size == expect.target.size);
            REQUIRE(target->chain == expect.target.chain);
            if (!target->is_directory)
            {
                REQUIRE(FatContents(device, *target) == expect.target_data);
            }
        }
        for (const std::uint32_t cluster : scan.lost)
        {
            REQUIRE(Contains(expect.target.chain, cluster));
        }
        // Some entry still carries the target's name.
        bool named = false;
        for (const FatEntryInfo* entry : sources)
        {
            named = named || !SameSpelling(entry->path, expect.old_path);
        }
        named = named || !targets.empty();
        REQUIRE(named);
    }
    else if (!expect.may_leak)
    {
        REQUIRE(scan.lost.empty());
    }

    if (completed)
    {
        REQUIRE(sources.size() == 1);
        REQUIRE(sources[0]->path == expect.new_path);
        if (!SameSpelling(expect.old_path, expect.new_path))
        {
            REQUIRE(scan.Find(expect.old_path) == nullptr);
        }
        REQUIRE(scan.lost.empty());
        REQUIRE(targets.empty());
        if (expect.replaces)
        {
            for (const std::uint32_t cluster : expect.target.chain)
            {
                REQUIRE(FatValue(device, cluster) == 0);
            }
        }
    }
}

// Files only: after an error the same mount still reads the source's data under one of its names.
void CheckLive(Filesystem& fat, const Expect& expect)
{
    if (expect.source.is_directory)
    {
        return;
    }
    bool reachable = false;
    for (const std::string& name : expect.names)
    {
        const Result<NodeId> node = Walk(fat, name);
        if (!node)
        {
            continue;
        }
        const std::string data = ReadAll(fat, *node);
        if (data == expect.source_data)
        {
            reachable = true;
        }
        else if (expect.replaces)
        {
            REQUIRE(data == expect.target_data);
        }
    }
    REQUIRE(reachable);
}

std::uint64_t CountWrites(MemoryDevice& base, const std::string& from, const std::string& to)
{
    const std::unique_ptr<MemoryDevice> device = Copy(base);
    MountedFilesystem fat(Fat16Type(), *device);
    const std::uint64_t before = device->write_count;
    Succeeds(RenameTo(*fat, from, to));
    return device->write_count - before;
}

using Extra = std::function<void(MemoryDevice&)>;

void EveryCut(MemoryDevice& base, const Expect& expect, const Extra& extra = {})
{
    const std::uint64_t writes = CountWrites(base, expect.old_path, expect.new_path);
    REQUIRE(writes > 0);
    for (std::uint64_t cut = 0; cut <= writes; ++cut)
    {
        INFO("stopped after write " << cut << " of " << writes);
        const std::unique_ptr<MemoryDevice> device = Copy(base);
        {
            MountedFilesystem fat(Fat16Type(), *device);
            device->FailWritesAfter(device->write_count + cut);
            const Status status = RenameTo(*fat, expect.old_path, expect.new_path);
            if (cut < writes)
            {
                REQUIRE(ErrorOf(status) == Error::Io);
                CheckLive(*fat, expect);
            }
            else
            {
                Succeeds(status);
            }
        }
        device->FailWritesAfter(UINT64_MAX);
        CheckState(*device, expect, cut == writes);
        if (extra && cut == writes)
        {
            extra(*device);
        }
    }
}

void MakeFile(Filesystem& fat, const std::string& path, const std::string& contents)
{
    WriteNewFile(fat, path, contents);
}

void MakeDirectory(Filesystem& fat, const std::string& path)
{
    const auto [parent, name] = Parent(fat, path);
    Value(fat.MakeDirectory(parent, name, 0755));
}

void Remove(Filesystem& fat, const std::string& path)
{
    const auto [parent, name] = Parent(fat, path);
    Succeeds(fat.Remove(parent, name));
}

void ExpectParent(MemoryDevice& device, const std::string& directory, const std::string& parent)
{
    MountedFilesystem fat(Fat16Type(), device, true);
    REQUIRE(Value(Walk(*fat, directory + "/..")) == Value(Walk(*fat, parent)));
}

bool SameImage(MemoryDevice& left, MemoryDevice& right)
{
    return std::memcmp(left.Bytes().data(), right.Bytes().data(), left.Bytes().size()) == 0;
}

// Fills a 16-entry root, of which used entries are taken, with files named g<number>.txt.
void FillRoot(Filesystem& fat, std::uint32_t used)
{
    for (std::uint32_t index = 0; used + index < 16; ++index)
    {
        MakeFile(fat, "g" + std::to_string(index) + ".txt", "g");
    }
}

} // namespace

TEST_CASE("FAT16 plain rename keeps the source at every write")
{
    SECTION("to a name that needs long-name entries")
    {
        const auto base = Prepared([](Filesystem& fat) { MakeFile(fat, "a.txt", Pattern(700)); });
        EveryCut(*base, Describe(*base, "a.txt", "Longer Name Here.txt", false));
    }
    SECTION("into another directory")
    {
        const auto base = Prepared(
            [](Filesystem& fat)
            {
                MakeDirectory(fat, "docs");
                MakeFile(fat, "a.txt", Pattern(700));
            });
        EveryCut(*base, Describe(*base, "a.txt", "docs/b.txt", false));
    }
    SECTION("a directory into another directory")
    {
        const auto base = Prepared(
            [](Filesystem& fat)
            {
                MakeDirectory(fat, "docs");
                MakeDirectory(fat, "kid");
            });
        EveryCut(*base, Describe(*base, "kid", "docs/kid2", false),
                 [](MemoryDevice& device) { ExpectParent(device, "docs/kid2", "docs"); });
    }
}

TEST_CASE("FAT16 respells a name in place and keeps the entry where it is")
{
    SECTION("a short name that gets a long name")
    {
        const auto base = Prepared(
            [](Filesystem& fat)
            {
                MakeFile(fat, "pad.txt", "p");
                MakeFile(fat, "readme.txt", Pattern(700));
                Remove(fat, "pad.txt");
            });
        EveryCut(*base, Describe(*base, "readme.txt", "Readme.txt", false, true));
    }
    SECTION("a long name in another case")
    {
        const auto base = Prepared([](Filesystem& fat)
                                   { MakeFile(fat, "A much longer name.txt", Pattern(700)); });
        EveryCut(*base,
                 Describe(*base, "A much longer name.txt", "a MUCH longer name.txt", false, true));
    }
    SECTION("a long name to its own short alias")
    {
        const auto base =
            Prepared([](Filesystem& fat) { MakeFile(fat, "Readme.txt", Pattern(700)); });
        EveryCut(*base, Describe(*base, "Readme.txt", "README~1.TXT", false, true));
    }
    SECTION("a long name respelled as a name that has another short name")
    {
        const auto base =
            Prepared([](Filesystem& fat) { MakeFile(fat, "Readme.txt", Pattern(700)); });
        EveryCut(*base, Describe(*base, "Readme.txt", "readme.txt", false, true));
    }
}

TEST_CASE("FAT16 moves a respelled name when it does not fit where it is")
{
    SECTION("the slot before is live")
    {
        const auto base = Prepared(
            [](Filesystem& fat)
            {
                MakeFile(fat, "x.txt", "x");
                MakeFile(fat, "readme.txt", Pattern(700));
            });
        EveryCut(*base, Describe(*base, "readme.txt", "Readme.txt", false));
    }
    SECTION("a subdirectory that has to grow")
    {
        const auto base = Prepared(
            [](Filesystem& fat)
            {
                MakeDirectory(fat, "docs");
                MakeFile(fat, "docs/x.txt", "x");
                MakeFile(fat, "docs/readme.txt", Pattern(700));
                for (int index = 0; index < 12; ++index)
                {
                    MakeFile(fat, "docs/f" + std::to_string(index) + ".txt", "f");
                }
            });
        Expect expect = Describe(*base, "docs/readme.txt", "docs/Readme.txt", false);
        expect.may_leak = true;
        EveryCut(*base, expect);
    }
}

TEST_CASE("FAT16 replacing rename keeps the source and the target's data at every write")
{
    SECTION("a file onto a file of the same spelling")
    {
        const auto base = Prepared(
            [](Filesystem& fat)
            {
                MakeFile(fat, "TARGET.TXT", Pattern(1300));
                MakeFile(fat, "new.txt", Pattern(900));
            });
        EveryCut(*base, Describe(*base, "new.txt", "TARGET.TXT", true));
    }
    SECTION("a respelling that fits")
    {
        const auto base = Prepared(
            [](Filesystem& fat)
            {
                MakeFile(fat, "pad.txt", "p");
                MakeFile(fat, "TARGET.TXT", Pattern(1300));
                MakeFile(fat, "new.txt", Pattern(900));
                Remove(fat, "pad.txt");
            });
        EveryCut(*base, Describe(*base, "new.txt", "Target.txt", true));
    }
    SECTION("a respelling that moves")
    {
        const auto base = Prepared(
            [](Filesystem& fat)
            {
                MakeFile(fat, "x.txt", "x");
                MakeFile(fat, "TARGET.TXT", Pattern(1300));
                MakeFile(fat, "new.txt", Pattern(900));
            });
        EveryCut(*base, Describe(*base, "new.txt", "Target.txt", true));
    }
    SECTION("a long-named target respelled in place")
    {
        const auto base = Prepared(
            [](Filesystem& fat)
            {
                MakeFile(fat, "A long target name.txt", Pattern(1300));
                MakeFile(fat, "new.txt", Pattern(900));
            });
        EveryCut(*base, Describe(*base, "new.txt", "a LONG target name.txt", true));
    }
    SECTION("an empty directory onto an empty directory")
    {
        const auto base = Prepared(
            [](Filesystem& fat)
            {
                MakeDirectory(fat, "dira");
                MakeDirectory(fat, "dirb");
            });
        EveryCut(*base, Describe(*base, "dira", "dirb", true));
    }
    SECTION("a file from another directory")
    {
        const auto base = Prepared(
            [](Filesystem& fat)
            {
                MakeDirectory(fat, "docs");
                MakeFile(fat, "target.txt", Pattern(1300));
                MakeFile(fat, "docs/new.txt", Pattern(900));
            });
        EveryCut(*base, Describe(*base, "docs/new.txt", "target.txt", true));
    }
    SECTION("a directory moved onto a directory of another parent")
    {
        const auto base = Prepared(
            [](Filesystem& fat)
            {
                MakeDirectory(fat, "docs");
                MakeDirectory(fat, "docs/dstdir");
                MakeDirectory(fat, "srcdir");
            });
        EveryCut(*base, Describe(*base, "srcdir", "docs/dstdir", true),
                 [](MemoryDevice& device) { ExpectParent(device, "docs/dstdir", "docs"); });
    }
}

TEST_CASE("FAT16 replacing rename in a full root fails whole and keeps the target")
{
    const auto base = Prepared(
        [](Filesystem& fat)
        {
            MakeFile(fat, "f0.txt", "f");
            MakeFile(fat, "README.TXT", Pattern(1300));
            for (int index = 1; index < 14; ++index)
            {
                MakeFile(fat, "f" + std::to_string(index) + ".txt", "f");
            }
            MakeFile(fat, "new.txt", Pattern(900));
        });
    const std::unique_ptr<MemoryDevice> device = Copy(*base);
    {
        MountedFilesystem fat(Fat16Type(), *device);
        const std::uint64_t writes = device->write_count;
        REQUIRE(ErrorOf(RenameTo(*fat, "new.txt", "Readme.txt")) == Error::NoSpace);
        REQUIRE(device->write_count == writes);
        REQUIRE(ReadAll(*fat, Value(Walk(*fat, "readme.txt"))) == Pattern(1300));
        REQUIRE(ReadAll(*fat, Value(Walk(*fat, "new.txt"))) == Pattern(900));
    }
    REQUIRE(SameImage(*device, *base));
}

TEST_CASE("FAT16 respelling in a full root fails whole, or fits in a deleted slot")
{
    SECTION("no room")
    {
        const auto base = Prepared(
            [](Filesystem& fat)
            {
                MakeFile(fat, "f0.txt", "f");
                MakeFile(fat, "readme.txt", Pattern(700));
                FillRoot(fat, 2);
            });
        const std::unique_ptr<MemoryDevice> device = Copy(*base);
        {
            MountedFilesystem fat(Fat16Type(), *device);
            const std::uint64_t writes = device->write_count;
            REQUIRE(ErrorOf(RenameTo(*fat, "readme.txt", "Readme.txt")) == Error::NoSpace);
            REQUIRE(device->write_count == writes);
            REQUIRE(ReadAll(*fat, Value(Walk(*fat, "readme.txt"))) == Pattern(700));
        }
        REQUIRE(SameImage(*device, *base));
    }
    SECTION("a deleted slot before the entry")
    {
        const auto base = Prepared(
            [](Filesystem& fat)
            {
                MakeFile(fat, "f0.txt", "f");
                MakeFile(fat, "readme.txt", Pattern(700));
                FillRoot(fat, 2);
                Remove(fat, "f0.txt");
            });
        const std::unique_ptr<MemoryDevice> device = Copy(*base);
        {
            MountedFilesystem fat(Fat16Type(), *device);
            Succeeds(RenameTo(*fat, "readme.txt", "Readme.txt"));
            REQUIRE(ReadAll(*fat, Value(Walk(*fat, "Readme.txt"))) == Pattern(700));
        }
        const FatScan scan = ScanFat16(*device);
        const FatEntryInfo* entry = scan.Find("Readme.txt");
        REQUIRE(entry != nullptr);
        REQUIRE(entry->path == "Readme.txt");
        REQUIRE(entry->slot == 1);
    }
}

TEST_CASE("FAT16 rename errors change nothing")
{
    const auto base = Prepared(
        [](Filesystem& fat)
        {
            MakeFile(fat, "file.txt", Pattern(700));
            MakeDirectory(fat, "dir");
            MakeDirectory(fat, "full");
            MakeFile(fat, "full/inside.txt", "inside");
            MakeDirectory(fat, "empty");
        });
    const std::unique_ptr<MemoryDevice> device = Copy(*base);
    {
        MountedFilesystem fat(Fat16Type(), *device);
        const NodeId root = fat->Root();
        REQUIRE(ErrorOf(fat->Rename(root, "empty", root, "full")) == Error::NotEmpty);
        REQUIRE(ErrorOf(fat->Rename(root, "file.txt", root, "dir")) == Error::IsDirectory);
        REQUIRE(ErrorOf(fat->Rename(root, "dir", root, "file.txt")) == Error::NotDirectory);
        REQUIRE(ErrorOf(RenameTo(*fat, "full", "full/inner")) == Error::InvalidArgument);
        REQUIRE(ErrorOf(fat->Rename(root, "file.txt", root, std::string(64, 'n'))) ==
                Error::NameTooLong);
        REQUIRE(ErrorOf(fat->Rename(root, "file.txt", root, "bad:name")) == Error::InvalidArgument);
        REQUIRE(ErrorOf(fat->Rename(root, "gone", root, "here")) == Error::NotFound);
        REQUIRE(device->write_count == 0);
    }
    REQUIRE(SameImage(*device, *base));
}

TEST_CASE("FAT16 rename with every short-name alias taken is refused before any write")
{
    Fat16Layout layout;
    layout.root_entries = 10240;
    Fat16Image image(layout);
    for (std::uint32_t number = 1; number < 10000; ++number)
    {
        Fat::ShortName name{};
        Fat::MakeBasis("Readme.txt", name);
        Fat::ApplyTail(name, number);
        std::uint8_t* entry = image.RootEntry(number - 1);
        std::memcpy(entry, name.data(), name.size());
        entry[11] = 0x20;
    }
    MountedFilesystem fat(Fat16Type(), image.Device());
    WriteNewFile(*fat, "a.txt", "a");
    const std::unique_ptr<MemoryDevice> before = Copy(image.Device());
    const std::uint64_t writes = image.Device().write_count;
    REQUIRE(ErrorOf(RenameTo(*fat, "a.txt", "Readme.txt")) == Error::Exists);
    REQUIRE(image.Device().write_count == writes);
    REQUIRE(SameImage(image.Device(), *before));
}

TEST_CASE("FAT16 renaming a name onto itself writes nothing")
{
    const auto base = Prepared(
        [](Filesystem& fat)
        {
            MakeFile(fat, "readme.txt", Pattern(700));
            MakeFile(fat, "A much longer name.txt", Pattern(300));
            MakeDirectory(fat, "Docs");
        });
    const std::unique_ptr<MemoryDevice> device = Copy(*base);
    {
        MountedFilesystem fat(Fat16Type(), *device);
        Succeeds(RenameTo(*fat, "readme.txt", "readme.txt"));
        Succeeds(RenameTo(*fat, "A much longer name.txt", "A much longer name.txt"));
        Succeeds(RenameTo(*fat, "Docs", "Docs"));
        REQUIRE(device->write_count == 0);
    }
    REQUIRE(SameImage(*device, *base));
}
