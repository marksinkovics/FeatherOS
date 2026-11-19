// SPDX-License-Identifier: MIT OR Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "common/little_endian.h"
#include "fat/names.h"
#include "fs/fat.h"
#include "fs/partition.h"
#include "tests/fat_image.h"
#include "tests/memory_device.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace
{

using FeatherDisk::Error;
using FeatherDisk::NodeId;
using FeatherDisk::NodeType;
using FeatherDisk::StoreLittleEndian;
using FeatherDisk::Fat::Fat16Type;
using FeatherDisk::Tests::ErrorOf;
using FeatherDisk::Tests::Fat16Image;
using FeatherDisk::Tests::Fat16Layout;
using FeatherDisk::Tests::FreeBlocks;
using FeatherDisk::Tests::List;
using FeatherDisk::Tests::MemoryDevice;
using FeatherDisk::Tests::MountedFilesystem;
using FeatherDisk::Tests::Pattern;
using FeatherDisk::Tests::ReadAll;
using FeatherDisk::Tests::Sorted;
using FeatherDisk::Tests::Succeeds;
using FeatherDisk::Tests::Value;
using FeatherDisk::Tests::Walk;
using FeatherDisk::Tests::WriteNewFile;

std::span<const std::uint8_t> Bytes(const std::string& text)
{
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

std::string Alphabet(std::size_t length)
{
    std::string text(length, '\0');
    for (std::size_t index = 0; index < length; ++index)
    {
        text[index] = static_cast<char>('A' + index % 26);
    }
    return text;
}

NodeId Lookup(FeatherDisk::Filesystem& filesystem, const char* path)
{
    return Value(Walk(filesystem, path));
}

// Marks every free cluster but count as used, behind the filesystem's back.
void LeaveFreeClusters(Fat16Image& image, std::uint32_t free_clusters, std::uint32_t count)
{
    std::uint32_t marked = 0;
    for (std::uint32_t cluster = 2; marked + count < free_clusters; ++cluster)
    {
        if (image.FatEntry(cluster) == 0)
        {
            image.SetFatEntry(cluster, 0xFFFF);
            ++marked;
        }
    }
}

} // namespace

TEST_CASE("FAT16 reads files across cluster boundaries")
{
    Fat16Image image;
    const std::string contents = Alphabet(700);
    image.AddRootFile(contents);
    MountedFilesystem fat(Fat16Type(), image.Device());

    const NodeId file = Value(fat->Lookup(fat->Root(), "BLOCK.TXT"));
    REQUIRE(Value(fat->GetAttributes(file)).size == contents.size());
    REQUIRE(ReadAll(*fat, file) == contents);

    std::array<std::uint8_t, 32> boundary{};
    REQUIRE(Value(fat->Read(file, 500, boundary)) == boundary.size());
    REQUIRE(std::string(boundary.begin(), boundary.end()) == contents.substr(500, boundary.size()));
    REQUIRE(Value(fat->Read(file, 690, boundary)) == 10);
    REQUIRE(Value(fat->Read(file, 700, boundary)) == 0);
    REQUIRE(Value(fat->Read(file, 9000, boundary)) == 0);
}

TEST_CASE("FAT16 probes and mounts only FAT16 volumes with 512-byte sectors")
{
    FeatherDisk::FilesystemType& type = Fat16Type();
    REQUIRE(type.Name() == "fat16");
    {
        Fat16Image image;
        REQUIRE(type.Probe(image.Device()));
    }
    {
        Fat16Image image;
        image.Device().Sector(0)[510] = 0;
        REQUIRE_FALSE(type.Probe(image.Device()));
        REQUIRE(ErrorOf(type.Mount(image.Device(), {})) == Error::Unsupported);
    }
    {
        Fat16Image image;
        StoreLittleEndian<std::uint16_t>(image.Device().Sector(0).data() + 11, 1024);
        REQUIRE_FALSE(type.Probe(image.Device()));
    }
    {
        Fat16Image image;
        image.Device().Sector(0)[13] = 3;
        REQUIRE_FALSE(type.Probe(image.Device()));
        image.Device().Sector(0)[13] = 0;
        REQUIRE_FALSE(type.Probe(image.Device()));
    }
    {
        // A boot sector claiming more sectors than the volume has.
        Fat16Image image;
        StoreLittleEndian<std::uint16_t>(image.Device().Sector(0).data() + 19, 6000);
        REQUIRE_FALSE(type.Probe(image.Device()));
    }
    {
        // Too few clusters for FAT16: FAT12.
        Fat16Image image({.sectors = 4000, .fat_count = 1, .fat_sectors = 20, .root_entries = 16});
        REQUIRE_FALSE(type.Probe(image.Device()));
    }
    {
        MemoryDevice large_sectors(4096, 1000);
        REQUIRE_FALSE(type.Probe(large_sectors));
    }
    {
        Fat16Image image;
        image.Device().FailReadsFrom(0);
        REQUIRE_FALSE(type.Probe(image.Device()));
        REQUIRE(ErrorOf(type.Mount(image.Device(), {})) == Error::Io);
    }
}

TEST_CASE("FAT16 reports missing names and the wrong kind of node")
{
    Fat16Image image;
    image.AddRootFile("contents");
    MountedFilesystem fat(Fat16Type(), image.Device());
    const NodeId file = Lookup(*fat, "BLOCK.TXT");

    REQUIRE(ErrorOf(fat->Lookup(fat->Root(), "MISSING.TXT")) == Error::NotFound);
    REQUIRE(ErrorOf(fat->Lookup(fat->Root(), std::string(64, 'n'))) == Error::NameTooLong);
    REQUIRE(ErrorOf(fat->Lookup(file, "x")) == Error::NotDirectory);
    std::uint64_t cookie = 0;
    FeatherDisk::DirectoryEntry entry{};
    REQUIRE(ErrorOf(fat->ReadDirectory(file, cookie, entry)) == Error::NotDirectory);
    std::array<std::uint8_t, 4> buffer{};
    REQUIRE(ErrorOf(fat->Read(fat->Root(), 0, buffer)) == Error::IsDirectory);
    REQUIRE(ErrorOf(fat->Create(file, "x", 0644)) == Error::NotDirectory);
    // A file ID whose entry is free.
    REQUIRE(ErrorOf(fat->GetAttributes(5)) == Error::NotFound);
}

TEST_CASE("FAT16 rejects corrupt cluster chains")
{
    Fat16Image image;
    image.AddRootFile(std::string(700, 'x'));
    image.SetFatEntry(2, 0xFFF7);
    MountedFilesystem fat(Fat16Type(), image.Device());
    const NodeId file = Lookup(*fat, "BLOCK.TXT");
    std::array<std::uint8_t, 700> contents{};
    REQUIRE(ErrorOf(fat->Read(file, 0, contents)) == Error::Corrupt);

    // A chain that ends before the size says.
    image.SetFatEntry(2, 0xFFFF);
    REQUIRE(ErrorOf(fat->Read(file, 0, contents)) == Error::Corrupt);
}

TEST_CASE("FAT16 creates a file and writes it across clusters")
{
    Fat16Image image;
    image.AddRootFile("contents");
    MountedFilesystem fat(Fat16Type(), image.Device());
    const std::uint32_t free_before = FreeBlocks(*fat);

    const NodeId file = Value(fat->Create(fat->Root(), "new.txt", 0644));
    REQUIRE(Value(fat->GetAttributes(file)).size == 0);
    const std::string contents = Pattern(1500);
    for (std::size_t offset = 0; offset < contents.size(); offset += 333)
    {
        const std::size_t chunk = std::min<std::size_t>(333, contents.size() - offset);
        REQUIRE(Value(fat->Write(file, offset, Bytes(contents).subspan(offset, chunk))) == chunk);
    }
    REQUIRE(Value(fat->GetAttributes(file)).size == contents.size());
    REQUIRE(FreeBlocks(*fat) == free_before - 3);
    REQUIRE(std::memcmp(image.RootEntry(1), "NEW     TXT", 11) == 0);
    REQUIRE(ReadAll(*fat, Lookup(*fat, "NEW.TXT")) == contents);
}

TEST_CASE("FAT16 overwrites in place and appends at a cluster boundary")
{
    Fat16Image image;
    image.AddRootFile("contents");
    MountedFilesystem fat(Fat16Type(), image.Device());
    std::string expected = Pattern(512);
    const NodeId file = WriteNewFile(*fat, "EDIT.BIN", expected);
    REQUIRE(Value(fat->Write(file, 512, Bytes("tail"))) == 4);
    expected += "tail";
    REQUIRE(Value(fat->Write(file, 100, Bytes("XYZ"))) == 3);
    expected.replace(100, 3, "XYZ");
    REQUIRE(ReadAll(*fat, file) == expected);
    REQUIRE(Value(fat->Write(file, 3, {})) == 0);
}

TEST_CASE("FAT16 truncation frees the clusters a file no longer needs")
{
    Fat16Image image;
    image.AddRootFile("contents");
    MountedFilesystem fat(Fat16Type(), image.Device());
    const std::string contents = Pattern(2000);
    const NodeId file = WriteNewFile(*fat, "SHRINK.TXT", contents);
    const std::uint32_t free_full = FreeBlocks(*fat);

    Succeeds(fat->Truncate(file, 600));
    REQUIRE(FreeBlocks(*fat) == free_full + 2);
    REQUIRE(ReadAll(*fat, file) == contents.substr(0, 600));
    Succeeds(fat->Truncate(file, 600));

    Succeeds(fat->Truncate(file, 0));
    REQUIRE(FreeBlocks(*fat) == free_full + 4);
    // An empty file owns no cluster.
    REQUIRE(FeatherDisk::LoadLittleEndian<std::uint16_t>(image.RootEntry(1) + 26) == 0);
    REQUIRE(ReadAll(*fat, file).empty());
}

TEST_CASE("FAT16 grows files with zeros, by truncation or by writing past the end")
{
    Fat16Image image;
    MountedFilesystem fat(Fat16Type(), image.Device());
    const NodeId file = WriteNewFile(*fat, "grow.bin", std::string(512, 'q'));
    const std::uint32_t free_before = FreeBlocks(*fat);

    // The cluster keeps its old bytes past the new end, which growing must not bring back.
    Succeeds(fat->Truncate(file, 10));
    Succeeds(fat->Truncate(file, 1000));
    std::string expected = std::string(10, 'q') + std::string(990, '\0');
    REQUIRE(ReadAll(*fat, file) == expected);
    REQUIRE(FreeBlocks(*fat) == free_before - 1);

    REQUIRE(Value(fat->Write(file, 1500, Bytes("xyz"))) == 3);
    expected += std::string(500, '\0') + "xyz";
    REQUIRE(ReadAll(*fat, file) == expected);
    REQUIRE(ErrorOf(fat->Truncate(file, 0x100000000ULL)) == Error::FileTooLarge);
}

TEST_CASE("FAT16 removal frees the chain and the directory slot")
{
    Fat16Image image;
    image.AddRootFile(std::string(700, 'x'));
    MountedFilesystem fat(Fat16Type(), image.Device());
    const std::uint32_t free_before = FreeBlocks(*fat);
    Succeeds(fat->Remove(fat->Root(), "BLOCK.TXT"));
    REQUIRE(ErrorOf(fat->Lookup(fat->Root(), "BLOCK.TXT")) == Error::NotFound);
    REQUIRE(FreeBlocks(*fat) == free_before + 2);
    REQUIRE(image.FatEntry(2) == 0);
    REQUIRE(image.FatEntry(3) == 0);
    REQUIRE(image.RootEntry(0)[0] == 0xE5);
    REQUIRE(ErrorOf(fat->Remove(fat->Root(), "BLOCK.TXT")) == Error::NotFound);

    // The root's first slot, directory cluster 0 and index 0, is node 0.
    REQUIRE(Value(fat->Create(fat->Root(), "AGAIN.TXT", 0644)) == 0);
}

TEST_CASE("FAT16 rejects bad names, duplicates, and a full root directory")
{
    Fat16Image image;
    image.AddRootFile("contents");
    MountedFilesystem fat(Fat16Type(), image.Device());
    const NodeId root = fat->Root();
    REQUIRE(ErrorOf(fat->Create(root, "BLOCK.TXT", 0644)) == Error::Exists);
    REQUIRE(ErrorOf(fat->Create(root, "block.txt", 0644)) == Error::Exists);
    REQUIRE(ErrorOf(fat->Create(root, "BAD*.TXT", 0644)) == Error::InvalidArgument);
    REQUIRE(ErrorOf(fat->Create(root, std::string(64, 'n'), 0644)) == Error::NameTooLong);
    REQUIRE(ErrorOf(fat->Create(root, "trailing.", 0644)) == Error::InvalidArgument);
    REQUIRE(ErrorOf(fat->Create(root, "a:b", 0644)) == Error::InvalidArgument);
    REQUIRE(ErrorOf(fat->Create(root, "", 0644)) == Error::InvalidArgument);
    REQUIRE(ErrorOf(fat->Create(root, "..", 0644)) == Error::Exists);
    REQUIRE(ErrorOf(fat->MakeDirectory(root, "bad|dir", 0755)) == Error::InvalidArgument);

    // The image's root directory has 16 entries, and BLOCK.TXT uses one.
    for (int index = 0; index < 15; ++index)
    {
        Value(fat->Create(root, "F" + std::to_string(index) + ".TXT", 0644));
    }
    REQUIRE(ErrorOf(fat->Create(root, "FULL.TXT", 0644)) == Error::NoSpace);
    // The new directory's cluster goes back when its entry does not fit.
    const std::uint32_t free_before = FreeBlocks(*fat);
    REQUIRE(ErrorOf(fat->MakeDirectory(root, "DIR", 0755)) == Error::NoSpace);
    REQUIRE(FreeBlocks(*fat) == free_before);
}

TEST_CASE("FAT16 stops a write cleanly when the volume is full")
{
    Fat16Image image;
    image.AddRootFile("contents");
    MountedFilesystem fat(Fat16Type(), image.Device());
    LeaveFreeClusters(image, FreeBlocks(*fat), 1);
    REQUIRE(FreeBlocks(*fat) == 1);

    const NodeId file = Value(fat->Create(fat->Root(), "FULL.TXT", 0644));
    const std::string contents = Pattern(1000);
    REQUIRE(Value(fat->Write(file, 0, Bytes(contents))) == 512);
    REQUIRE(Value(fat->GetAttributes(file)).size == 512);
    REQUIRE(ReadAll(*fat, file) == contents.substr(0, 512));
    REQUIRE(ErrorOf(fat->Write(file, 512, Bytes(contents))) == Error::NoSpace);
    REQUIRE(ErrorOf(fat->Truncate(file, 2000)) == Error::NoSpace);
}

TEST_CASE("FAT16 refuses to free a looping cluster chain")
{
    Fat16Image image;
    image.AddRootFile(std::string(700, 'x'));
    image.SetFatEntry(3, 2);
    MountedFilesystem fat(Fat16Type(), image.Device());
    REQUIRE(ErrorOf(fat->Remove(fat->Root(), "BLOCK.TXT")) == Error::Corrupt);
}

TEST_CASE("FAT16 creates nested directories and files inside them")
{
    Fat16Image image;
    image.AddRootFile("contents");
    MountedFilesystem fat(Fat16Type(), image.Device());
    const NodeId root = fat->Root();
    const NodeId docs = Value(fat->MakeDirectory(root, "docs", 0755));
    REQUIRE(Lookup(*fat, "DOCS") == docs);
    const NodeId old = Value(fat->MakeDirectory(docs, "old", 0755));
    REQUIRE(ErrorOf(fat->MakeDirectory(root, "docs", 0755)) == Error::Exists);
    REQUIRE(ErrorOf(Walk(*fat, "missing/sub")) == Error::NotFound);
    WriteNewFile(*fat, "docs/notes.txt", Pattern(700));
    WriteNewFile(*fat, "docs/old/a.txt", "deep");

    REQUIRE(List(*fat, root) == std::vector<std::string>{"BLOCK.TXT", "docs/"});
    REQUIRE(List(*fat, docs) == std::vector<std::string>{"old/", "notes.txt"});
    REQUIRE(List(*fat, old) == std::vector<std::string>{"a.txt"});
    REQUIRE(ReadAll(*fat, Lookup(*fat, "docs/notes.txt")) == Pattern(700));
    REQUIRE(ReadAll(*fat, Lookup(*fat, "docs/old/../old/./a.txt")) == "deep");
    REQUIRE(ReadAll(*fat, Lookup(*fat, "docs/old/../../BLOCK.TXT")) == "contents");
    REQUIRE(ReadAll(*fat, Lookup(*fat, "../BLOCK.TXT")) == "contents");
    REQUIRE(Value(fat->Lookup(old, "..")) == docs);
    REQUIRE(Value(fat->Lookup(docs, "..")) == root);
    REQUIRE(Value(fat->Lookup(root, ".")) == root);
    REQUIRE(Value(fat->GetAttributes(docs)).type == NodeType::Directory);

    // A directory is not a file, and a file is not a directory.
    REQUIRE(ErrorOf(fat->Remove(root, "docs")) == Error::IsDirectory);
    REQUIRE(ErrorOf(Walk(*fat, "BLOCK.TXT/x")) == Error::NotDirectory);
    REQUIRE(ErrorOf(fat->RemoveDirectory(root, "BLOCK.TXT")) == Error::NotDirectory);
}

TEST_CASE("FAT16 grows a subdirectory past its first cluster")
{
    Fat16Image image;
    MountedFilesystem fat(Fat16Type(), image.Device());
    const NodeId many = Value(fat->MakeDirectory(fat->Root(), "many", 0755));
    const std::uint32_t free_before = FreeBlocks(*fat);
    // One-sector clusters hold 16 entries, and "." and ".." take two of them.
    std::vector<std::string> expected;
    for (int index = 0; index < 40; ++index)
    {
        const std::string name = "F" + std::to_string(index) + ".TXT";
        Value(fat->Create(many, name, 0644));
        expected.push_back(name);
    }
    REQUIRE(FreeBlocks(*fat) == free_before - 2);
    REQUIRE(List(*fat, many) == expected);
    REQUIRE(ReadAll(*fat, Lookup(*fat, "many/F39.TXT")).empty());
}

TEST_CASE("FAT16 removes only empty directories and frees their clusters")
{
    Fat16Image image;
    image.AddRootFile("contents");
    MountedFilesystem fat(Fat16Type(), image.Device());
    const NodeId root = fat->Root();
    const std::uint32_t free_before = FreeBlocks(*fat);
    const NodeId scratch = Value(fat->MakeDirectory(root, "scratch", 0755));
    WriteNewFile(*fat, "scratch/x.txt", "x");
    REQUIRE(ErrorOf(fat->RemoveDirectory(root, "scratch")) == Error::NotEmpty);
    Succeeds(fat->Remove(scratch, "x.txt"));
    Succeeds(fat->RemoveDirectory(root, "SCRATCH"));
    REQUIRE(FreeBlocks(*fat) == free_before);
    REQUIRE(List(*fat, root) == std::vector<std::string>{"BLOCK.TXT"});
    REQUIRE(ErrorOf(fat->RemoveDirectory(root, "scratch")) == Error::NotFound);
    REQUIRE(ErrorOf(fat->RemoveDirectory(root, ".")) == Error::InvalidArgument);
    REQUIRE(ErrorOf(fat->RemoveDirectory(root, "..")) == Error::InvalidArgument);
}

TEST_CASE("FAT16 renames files and directories, keeping their data")
{
    Fat16Image image;
    image.AddRootFile("contents");
    MountedFilesystem fat(Fat16Type(), image.Device());
    const NodeId root = fat->Root();
    WriteNewFile(*fat, "a.txt", Pattern(700));
    WriteNewFile(*fat, "c.txt", "c");
    const NodeId docs = Value(fat->MakeDirectory(root, "docs", 0755));
    const NodeId old = Value(fat->MakeDirectory(docs, "old", 0755));
    WriteNewFile(*fat, "docs/old/x.txt", "deep");
    const std::uint32_t free_before = FreeBlocks(*fat);

    Succeeds(fat->Rename(root, "a.txt", root, "b.txt"));
    Succeeds(fat->Rename(root, "b.txt", root, "A much longer name.txt"));
    Succeeds(fat->Rename(root, "a much longer NAME.txt", root, "a MUCH longer name.txt"));
    Succeeds(fat->Rename(root, "c.txt", root, "C.TXT"));
    REQUIRE(ReadAll(*fat, Lookup(*fat, "a MUCH longer name.txt")) == Pattern(700));
    REQUIRE(Sorted(List(*fat, root)) ==
            std::vector<std::string>{"BLOCK.TXT", "C.TXT", "a MUCH longer name.txt", "docs/"});

    // A moved directory keeps its node, and its ".." follows it.
    Succeeds(fat->Rename(docs, "old", root, "old"));
    REQUIRE(Lookup(*fat, "old") == old);
    REQUIRE(Value(fat->Lookup(old, "..")) == root);
    Succeeds(fat->Rename(root, "old", docs, "archive"));
    REQUIRE(Lookup(*fat, "docs/archive") == old);
    REQUIRE(ReadAll(*fat, Lookup(*fat, "docs/archive/x.txt")) == "deep");
    REQUIRE(ReadAll(*fat, Lookup(*fat, "docs/archive/../../BLOCK.TXT")) == "contents");
    REQUIRE(List(*fat, docs) == std::vector<std::string>{"archive/"});
    REQUIRE(FreeBlocks(*fat) == free_before);

    REQUIRE(ErrorOf(fat->Rename(root, "docs", old, "inner")) == Error::InvalidArgument);
    REQUIRE(ErrorOf(fat->Rename(root, "docs", docs, "inner")) == Error::InvalidArgument);
    REQUIRE(ErrorOf(fat->Rename(root, "C.TXT", root, "bad:name")) == Error::InvalidArgument);
    REQUIRE(ErrorOf(fat->Rename(root, "gone", root, "here")) == Error::NotFound);
    REQUIRE(ErrorOf(fat->Rename(docs, "..", root, "up")) == Error::InvalidArgument);
    REQUIRE(ErrorOf(fat->Rename(root, "C.TXT", docs, "..")) == Error::InvalidArgument);
    REQUIRE(ReadAll(*fat, Lookup(*fat, "C.TXT")) == "c");
}

TEST_CASE("FAT16 rename replaces a target of the same kind")
{
    Fat16Image image;
    MountedFilesystem fat(Fat16Type(), image.Device());
    const NodeId root = fat->Root();
    WriteNewFile(*fat, "a.txt", "a");
    WriteNewFile(*fat, "b.txt", Pattern(700));
    const std::uint32_t free_before = FreeBlocks(*fat);
    Succeeds(fat->Rename(root, "a.txt", root, "B.TXT"));
    REQUIRE(ReadAll(*fat, Lookup(*fat, "b.txt")) == "a");
    REQUIRE(ErrorOf(fat->Lookup(root, "a.txt")) == Error::NotFound);
    REQUIRE(FreeBlocks(*fat) == free_before + 2);

    Value(fat->MakeDirectory(root, "x", 0755));
    const NodeId y = Value(fat->MakeDirectory(root, "y", 0755));
    const NodeId z = Value(fat->MakeDirectory(root, "z", 0755));
    WriteNewFile(*fat, "z/inside.txt", "inside");
    REQUIRE(ErrorOf(fat->Rename(root, "y", root, "z")) == Error::NotEmpty);
    REQUIRE(ErrorOf(fat->Rename(root, "b.txt", root, "x")) == Error::IsDirectory);
    REQUIRE(ErrorOf(fat->Rename(root, "x", root, "b.txt")) == Error::NotDirectory);
    REQUIRE(ErrorOf(fat->Rename(z, "inside.txt", root, "z")) == Error::IsDirectory);
    Succeeds(fat->Rename(root, "y", root, "x"));
    REQUIRE(Lookup(*fat, "x") == y);
    REQUIRE(Sorted(List(*fat, root)) == std::vector<std::string>{"B.TXT", "x/", "z/"});
}

TEST_CASE("FAT16 stores long names with numbered short aliases")
{
    Fat16Image image;
    image.AddRootFile("contents");
    MountedFilesystem fat(Fat16Type(), image.Device());
    WriteNewFile(*fat, "My Notes.txt", "first");
    WriteNewFile(*fat, "My Notes2.txt", "second");
    // Slot 1 holds the long-name entry and slot 2 the short alias.
    REQUIRE(image.RootEntry(1)[11] == FeatherDisk::Fat::LongNameAttribute);
    REQUIRE(image.RootEntry(1)[0] == 0x41);
    REQUIRE(std::memcmp(image.RootEntry(2), "MYNOTE~1TXT", 11) == 0);
    REQUIRE(image.RootEntry(1)[13] == FeatherDisk::Fat::Checksum(image.RootEntry(2)));
    REQUIRE(std::memcmp(image.RootEntry(4), "MYNOTE~2TXT", 11) == 0);

    REQUIRE(List(*fat, fat->Root()) ==
            std::vector<std::string>{"BLOCK.TXT", "My Notes.txt", "My Notes2.txt"});
    REQUIRE(ReadAll(*fat, Lookup(*fat, "my notes.txt")) == "first");
    REQUIRE(ReadAll(*fat, Lookup(*fat, "MYNOTE~1.TXT")) == "first");
    REQUIRE(ReadAll(*fat, Lookup(*fat, "MY NOTES2.TXT")) == "second");
    REQUIRE(ErrorOf(fat->Create(fat->Root(), "mynote~1.txt", 0644)) == Error::Exists);
    REQUIRE(ErrorOf(fat->Create(fat->Root(), "MY NOTES.TXT", 0644)) == Error::Exists);
}

TEST_CASE("FAT16 keeps lower-case 8.3 names as short entries")
{
    Fat16Image image;
    image.AddRootFile("contents");
    MountedFilesystem fat(Fat16Type(), image.Device());
    WriteNewFile(*fat, "readme.md", "text");
    REQUIRE(std::memcmp(image.RootEntry(1), "README  MD ", 11) == 0);
    REQUIRE(image.RootEntry(1)[12] == 0x18);
    REQUIRE(List(*fat, fat->Root()) == std::vector<std::string>{"BLOCK.TXT", "readme.md"});
    REQUIRE(ReadAll(*fat, Lookup(*fat, "README.MD")) == "text");
}

TEST_CASE("FAT16 removes a long-named file's name entries")
{
    Fat16Image image;
    image.AddRootFile("contents");
    MountedFilesystem fat(Fat16Type(), image.Device());
    WriteNewFile(*fat, "a much longer file name.text", "x");
    Succeeds(fat->Remove(fat->Root(), "A MUCH LONGER FILE NAME.TEXT"));
    for (std::uint32_t slot = 1; slot <= 4; ++slot)
    {
        REQUIRE(image.RootEntry(slot)[0] == 0xE5);
    }
    REQUIRE(List(*fat, fat->Root()) == std::vector<std::string>{"BLOCK.TXT"});
    WriteNewFile(*fat, "another long name.txt", "y");
    REQUIRE(List(*fat, fat->Root()) ==
            std::vector<std::string>{"BLOCK.TXT", "another long name.txt"});
}

TEST_CASE("FAT16 long-named directories grow to fit a name's entries")
{
    Fat16Image image;
    image.AddRootFile("contents");
    MountedFilesystem fat(Fat16Type(), image.Device());
    Value(fat->MakeDirectory(fat->Root(), "My Documents", 0755));
    // "." and ".." plus ten short files leave four free slots in the first cluster, one too few
    // for a name needing four long-name entries and its short entry.
    for (int index = 0; index < 10; ++index)
    {
        WriteNewFile(*fat, "my documents/f" + std::to_string(index), "");
    }
    const std::string long_name = "a name that needs four long entries.txt";
    WriteNewFile(*fat, "My Documents/" + long_name, "deep");
    REQUIRE(ReadAll(*fat, Lookup(*fat, ("my documents/" + long_name).c_str())) == "deep");
    REQUIRE(List(*fat, Lookup(*fat, "My Documents")).back() == long_name);
    REQUIRE(List(*fat, fat->Root()) == std::vector<std::string>{"BLOCK.TXT", "My Documents/"});
    REQUIRE(ErrorOf(fat->RemoveDirectory(fat->Root(), "my documents")) == Error::NotEmpty);
}

TEST_CASE("FAT16 updates every FAT copy")
{
    Fat16Image image({.sectors = 5000, .fat_count = 2, .fat_sectors = 20, .root_entries = 16});
    image.AddRootFile(Pattern(700));
    MountedFilesystem fat(Fat16Type(), image.Device());
    WriteNewFile(*fat, "copies.txt", Pattern(3000));
    Value(fat->MakeDirectory(fat->Root(), "dir", 0755));
    Succeeds(fat->Remove(fat->Root(), "BLOCK.TXT"));
    for (std::uint32_t cluster = 0; cluster < 5120; ++cluster)
    {
        REQUIRE(image.FatEntry(cluster, 0) == image.FatEntry(cluster, 1));
    }
    REQUIRE(image.FatEntry(2) == 0);
}

TEST_CASE("FAT16 mounted read-only refuses every change")
{
    Fat16Image image;
    image.AddRootFile("contents");
    MountedFilesystem fat(Fat16Type(), image.Device(), true);
    const NodeId root = fat->Root();
    const NodeId file = Lookup(*fat, "BLOCK.TXT");
    REQUIRE_FALSE(fat->GetCapabilities().writable);
    REQUIRE(Value(fat->GetAttributes(file)).mode == 0444);
    REQUIRE(Value(fat->GetAttributes(root)).mode == 0555);
    REQUIRE(ReadAll(*fat, file) == "contents");
    REQUIRE(ErrorOf(fat->Create(root, "new.txt", 0644)) == Error::ReadOnly);
    REQUIRE(ErrorOf(fat->MakeDirectory(root, "dir", 0755)) == Error::ReadOnly);
    REQUIRE(ErrorOf(fat->Write(file, 0, Bytes("x"))) == Error::ReadOnly);
    REQUIRE(ErrorOf(fat->Truncate(file, 0)) == Error::ReadOnly);
    REQUIRE(ErrorOf(fat->Remove(root, "BLOCK.TXT")) == Error::ReadOnly);
    REQUIRE(ErrorOf(fat->Rename(root, "BLOCK.TXT", root, "b.txt")) == Error::ReadOnly);
    REQUIRE(ErrorOf(fat->SetAttributes(file, {})) == Error::Unsupported);
    REQUIRE(ErrorOf(fat->SymbolicLink(root, "link", "BLOCK.TXT")) == Error::Unsupported);
}

TEST_CASE("FAT16 node IDs stay with a file while it is written")
{
    Fat16Image image;
    MountedFilesystem fat(Fat16Type(), image.Device());
    const NodeId file = Value(fat->Create(fat->Root(), "id.txt", 0644));
    REQUIRE(Value(fat->Write(file, 0, Bytes(Pattern(1200)))) == 1200);
    Succeeds(fat->Truncate(file, 0));
    REQUIRE(Value(fat->Write(file, 0, Bytes("again"))) == 5);
    REQUIRE(Lookup(*fat, "ID.TXT") == file);
    const FeatherDisk::Attributes attributes = Value(fat->GetAttributes(file));
    REQUIRE(attributes.node == file);
    REQUIRE(attributes.type == NodeType::File);
    REQUIRE(attributes.size == 5);
    REQUIRE(attributes.mode == 0644);
    REQUIRE(Value(fat->GetAttributes(fat->Root())).mode == 0755);
    REQUIRE(attributes.link_count == 1);
}

TEST_CASE("FAT16 lists a directory by cookie, without dot entries or deleted files")
{
    Fat16Image image;
    MountedFilesystem fat(Fat16Type(), image.Device());
    const NodeId docs = Value(fat->MakeDirectory(fat->Root(), "docs", 0755));
    WriteNewFile(*fat, "docs/gone.txt", "");
    const NodeId kept = WriteNewFile(*fat, "docs/Kept File.txt", "kept");
    const NodeId sub = Value(fat->MakeDirectory(docs, "sub", 0755));
    Succeeds(fat->Remove(docs, "gone.txt"));

    std::uint64_t cookie = 0;
    FeatherDisk::DirectoryEntry entry{};
    REQUIRE(Value(fat->ReadDirectory(docs, cookie, entry)));
    REQUIRE(std::string(entry.name.data(), entry.name_length) == "Kept File.txt");
    REQUIRE(entry.node == kept);
    REQUIRE(entry.type == NodeType::File);
    REQUIRE(Value(fat->ReadDirectory(docs, cookie, entry)));
    REQUIRE(std::string(entry.name.data(), entry.name_length) == "sub");
    REQUIRE(entry.node == sub);
    REQUIRE(entry.type == NodeType::Directory);
    REQUIRE_FALSE(Value(fat->ReadDirectory(docs, cookie, entry)));
    REQUIRE_FALSE(Value(fat->ReadDirectory(docs, cookie, entry)));
}

TEST_CASE("FAT16 reports its capabilities and counts clusters")
{
    Fat16Image image;
    image.AddRootFile(Pattern(700));
    MountedFilesystem fat(Fat16Type(), image.Device());
    const FeatherDisk::Capabilities capabilities = fat->GetCapabilities();
    REQUIRE(capabilities.writable);
    REQUIRE_FALSE(capabilities.case_sensitive);
    REQUIRE_FALSE(capabilities.symbolic_links);
    REQUIRE_FALSE(capabilities.hard_links);
    REQUIRE_FALSE(capabilities.permissions);
    REQUIRE(capabilities.maximum_name_length == 63);
    REQUIRE(capabilities.maximum_file_size == 0xFFFFFFFFU);
    const FeatherDisk::VolumeStatistics statistics = Value(fat->Statistics());
    REQUIRE(statistics.block_size == 512);
    // 5000 sectors less the boot sector, 20 FAT sectors and one root directory sector.
    REQUIRE(statistics.total_blocks == 4978);
    REQUIRE(statistics.free_blocks == 4976);
    REQUIRE(statistics.total_nodes == 0);
}

TEST_CASE("FAT16 sync and unmount flush the device")
{
    Fat16Image image;
    MemoryDevice& device = image.Device();
    FeatherDisk::Filesystem* filesystem = Value(Fat16Type().Mount(device, {}));
    Succeeds(filesystem->Sync());
    REQUIRE(device.flush_count == 1);
    WriteNewFile(*filesystem, "kept.txt", "kept");
    Succeeds(Fat16Type().Unmount(*filesystem));
    REQUIRE(device.flush_count == 2);

    // Everything was on the volume already, so a new mount sees it.
    MountedFilesystem again(Fat16Type(), device);
    REQUIRE(ReadAll(*again, Lookup(*again, "kept.txt")) == "kept");
}

TEST_CASE("FAT16 MakeReadOnly flushes once, refuses writes and keeps reads and a later mount")
{
    Fat16Image image;
    image.AddRootFile("contents");
    MemoryDevice& device = image.Device();
    FeatherDisk::Filesystem* filesystem = Value(Fat16Type().Mount(device, {}));
    WriteNewFile(*filesystem, "first.txt", "first");
    const std::uint32_t flushes = device.flush_count;
    const std::uint64_t writes = device.write_count;
    Succeeds(filesystem->MakeReadOnly());
    REQUIRE(device.flush_count == flushes + 1);
    REQUIRE(device.write_count == writes);
    REQUIRE_FALSE(filesystem->GetCapabilities().writable);
    const NodeId root = filesystem->Root();
    REQUIRE(ReadAll(*filesystem, Lookup(*filesystem, "first.txt")) == "first");
    REQUIRE(ReadAll(*filesystem, Lookup(*filesystem, "BLOCK.TXT")) == "contents");
    REQUIRE(ErrorOf(filesystem->Create(root, "second.txt", 0644)) == Error::ReadOnly);
    REQUIRE(ErrorOf(filesystem->Write(Lookup(*filesystem, "first.txt"), 0, Bytes("x"))) ==
            Error::ReadOnly);
    REQUIRE(ErrorOf(filesystem->Remove(root, "first.txt")) == Error::ReadOnly);
    Succeeds(filesystem->MakeReadOnly());
    Succeeds(Fat16Type().Unmount(*filesystem));
    REQUIRE(device.write_count == writes);

    MountedFilesystem again(Fat16Type(), device);
    WriteNewFile(*again, "second.txt", "second");
    REQUIRE(ReadAll(*again, Lookup(*again, "first.txt")) == "first");
    REQUIRE(ReadAll(*again, Lookup(*again, "second.txt")) == "second");
}

TEST_CASE("FAT16 MakeReadOnly on a read-only mount writes nothing")
{
    Fat16Image image;
    image.AddRootFile("contents");
    MountedFilesystem fat(Fat16Type(), image.Device(), true);
    Succeeds(fat->MakeReadOnly());
    REQUIRE(image.Device().write_count == 0);
    REQUIRE(ReadAll(*fat, Lookup(*fat, "BLOCK.TXT")) == "contents");
}

TEST_CASE("FAT16 mounts a partition through a volume")
{
    Fat16Image image;
    image.AddRootFile("partitioned");
    MemoryDevice disk(512, 5001);
    std::uint8_t* entry = disk.Sector(0).data() + 446;
    entry[4] = 0x06;
    StoreLittleEndian<std::uint32_t>(entry + 8, 1);
    StoreLittleEndian<std::uint32_t>(entry + 12, 5000);
    disk.Sector(0)[510] = 0x55;
    disk.Sector(0)[511] = 0xAA;
    for (std::uint64_t sector = 0; sector < 5000; ++sector)
    {
        std::copy_n(image.Device().Sector(sector).data(), 512, disk.Sector(sector + 1).data());
    }

    std::array<FeatherDisk::Partition::Range, 4> ranges{};
    REQUIRE(FeatherDisk::Partition::Find(disk, ranges) == 1);
    FeatherDisk::Partition::Volume volume(disk, ranges[0]);
    REQUIRE_FALSE(Fat16Type().Probe(disk));
    REQUIRE(Fat16Type().Probe(volume));
    MountedFilesystem fat(Fat16Type(), volume);
    REQUIRE(ReadAll(*fat, Lookup(*fat, "BLOCK.TXT")) == "partitioned");
    WriteNewFile(*fat, "new.txt", "x");
    // The volume's sector 0 is the disk's sector 1.
    REQUIRE(std::memcmp(disk.Sector(22).data() + 32, "NEW     TXT", 11) == 0);
}

TEST_CASE("partition volumes offset sectors and stay inside the partition")
{
    MemoryDevice disk(512, 64);
    disk.Sector(10)[0] = 0xAB;
    FeatherDisk::Partition::Volume volume(disk, {.first_sector = 10, .sector_count = 20});
    REQUIRE(volume.SectorSize() == 512);
    REQUIRE(volume.SectorCount() == 20);
    std::vector<std::uint8_t> sector(512);
    Succeeds(volume.Read(0, sector));
    REQUIRE(sector[0] == 0xAB);
    sector[0] = 0xCD;
    Succeeds(volume.Write(19, sector));
    REQUIRE(disk.Sector(29)[0] == 0xCD);
    REQUIRE(volume.Write(20, sector).error() == Error::InvalidArgument);
    std::vector<std::uint8_t> two(1024);
    REQUIRE(volume.Read(19, two).error() == Error::InvalidArgument);
    REQUIRE(volume.Read(0, std::span(two).first(100)).error() == Error::InvalidArgument);
    Succeeds(volume.Flush());
    REQUIRE(disk.flush_count == 1);

    FeatherDisk::Partition::Volume empty;
    REQUIRE(empty.SectorCount() == 0);
    REQUIRE(empty.Read(0, sector).error() == Error::InvalidArgument);
    empty.Assign(disk, {.first_sector = 29, .sector_count = 1});
    Succeeds(empty.Read(0, sector));
    REQUIRE(sector[0] == 0xCD);
}
