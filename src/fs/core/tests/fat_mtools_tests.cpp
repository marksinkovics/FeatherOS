// SPDX-License-Identifier: MIT OR Apache-2.0
// FAT16 against mtools, an independent implementation: the core reads the image
// tests/make_fat16_image.sh made, and mtools reads back what the core wrote to it.
#include <catch2/catch_test_macros.hpp>

#include "common/little_endian.h"
#include "fs/fat.h"
#include "tests/fat_image.h"
#include "tests/memory_device.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <vector>

#ifdef FEATHERDISK_MTOOLS_IMAGE
namespace
{

using FeatherDisk::NodeId;
using FeatherDisk::Fat::Fat16Type;
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

constexpr std::uint32_t SectorSize = 512;

std::string ReadHostFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.good());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::unique_ptr<MemoryDevice> LoadImage()
{
    const std::string bytes = ReadHostFile(FEATHERDISK_MTOOLS_IMAGE);
    REQUIRE(bytes.size() % SectorSize == 0);
    auto device = std::make_unique<MemoryDevice>(SectorSize, bytes.size() / SectorSize);
    for (std::size_t sector = 0; sector < bytes.size() / SectorSize; ++sector)
    {
        std::memcpy(device->Sector(sector).data(), bytes.data() + sector * SectorSize, SectorSize);
    }
    return device;
}

void SaveImage(MemoryDevice& device, const std::string& path)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    for (std::uint64_t sector = 0; sector < device.SectorCount(); ++sector)
    {
        file.write(reinterpret_cast<const char*>(device.Sector(sector).data()), SectorSize);
    }
    REQUIRE(file.good());
}

// Runs an mtools program on image and returns what it printed.
std::string Mtools(const char* program, const std::string& image, const std::string& arguments)
{
    const std::string output = image + ".mtools-output.txt";
    const std::string command = std::string("\"") + program + "\" -i \"" + image + "\" " +
                                arguments + " > \"" + output + "\"";
    REQUIRE(std::system(command.c_str()) == 0);
    return ReadHostFile(output);
}

std::string BigFile()
{
    std::string contents;
    for (int line = 0; line < 300; ++line)
    {
        char text[64];
        std::snprintf(text, sizeof(text), "line %03d of a file over many clusters\n", line);
        contents += text;
    }
    return contents;
}

} // namespace
#endif

TEST_CASE("FAT16 reads an image that mtools made")
{
#ifndef FEATHERDISK_MTOOLS_IMAGE
    SKIP("mtools is not installed");
#else
    std::unique_ptr<MemoryDevice> device = LoadImage();
    REQUIRE(Fat16Type().Probe(*device));
    MountedFilesystem fat(Fat16Type(), *device);

    REQUIRE(Sorted(List(*fat, fat->Root())) ==
            std::vector<std::string>{"Read Me First.txt", "big.txt", "docs/", "many/"});
    REQUIRE(ReadAll(*fat, Value(Walk(*fat, "read me first.txt"))) ==
            "Long names from mtools work.\n");
    REQUIRE(ReadAll(*fat, Value(Walk(*fat, "README~1.TXT"))) == "Long names from mtools work.\n");
    REQUIRE(ReadAll(*fat, Value(Walk(*fat, "big.txt"))) == BigFile());
    REQUIRE(ReadAll(*fat, Value(Walk(*fat, "DOCS/NOTES.TXT"))) == "notes\n");
    REQUIRE(ReadAll(*fat, Value(Walk(*fat, "docs/archive/Old Plans.txt"))) == "old plans\n");
    REQUIRE(Value(Walk(*fat, "docs/archive/..")) == Value(Walk(*fat, "docs")));

    std::vector<std::string> expected;
    for (int number = 0; number < 40; ++number)
    {
        expected.push_back("f" + std::to_string(number) + ".txt");
    }
    REQUIRE(Sorted(List(*fat, Value(Walk(*fat, "many")))) == Sorted(expected));
    REQUIRE(ReadAll(*fat, Value(Walk(*fat, "many/f39.txt"))) == "file 39\n");

    const FeatherDisk::VolumeStatistics statistics = Value(fat->Statistics());
    REQUIRE(statistics.block_size == 512);
    REQUIRE(statistics.free_blocks < statistics.total_blocks);
#endif
}

TEST_CASE("mtools reads back what FAT16 wrote")
{
#ifndef FEATHERDISK_MTOOLS_IMAGE
    SKIP("mtools is not installed");
#else
    std::unique_ptr<MemoryDevice> device = LoadImage();
    const std::string plans = Pattern(3000);
    {
        MountedFilesystem fat(Fat16Type(), *device);
        const NodeId root = fat->Root();
        WriteNewFile(*fat, "docs/My Plans.txt", plans);
        const NodeId folder = Value(fat->MakeDirectory(root, "New Folder", 0755));
        WriteNewFile(*fat, "New Folder/inner.txt", "inner\n");
        Succeeds(fat->Remove(root, "big.txt"));
        Succeeds(fat->Rename(Value(Walk(*fat, "docs")), "notes.txt", folder, "Renamed Notes.txt"));
        const NodeId readme = Value(Walk(*fat, "Read Me First.txt"));
        const std::string more = "More from FeatherDisk.\n";
        const std::span<const std::uint8_t> data(reinterpret_cast<const std::uint8_t*>(more.data()),
                                                 more.size());
        REQUIRE(Value(fat->Write(readme, 29, data)) == more.size());
        Value(fat->MakeDirectory(root, "empty", 0755));
        Succeeds(fat->RemoveDirectory(root, "empty"));
    }

    const std::uint8_t* boot = device->Sector(0).data();
    const auto reserved = FeatherDisk::LoadLittleEndian<std::uint16_t>(boot + 14);
    const auto fat_sectors = FeatherDisk::LoadLittleEndian<std::uint16_t>(boot + 22);
    REQUIRE(boot[16] == 2);
    for (std::uint32_t sector = 0; sector < fat_sectors; ++sector)
    {
        REQUIRE(std::memcmp(device->Sector(reserved + sector).data(),
                            device->Sector(reserved + fat_sectors + sector).data(),
                            SectorSize) == 0);
    }

    const std::string image = std::string(FEATHERDISK_TEST_OUTPUT) + "/fat16-written.img";
    SaveImage(*device, image);
    REQUIRE(Mtools(FEATHERDISK_MTYPE, image, "\"::docs/My Plans.txt\"") == plans);
    REQUIRE(Mtools(FEATHERDISK_MTYPE, image, "\"::New Folder/inner.txt\"") == "inner\n");
    REQUIRE(Mtools(FEATHERDISK_MTYPE, image, "\"::New Folder/Renamed Notes.txt\"") == "notes\n");
    REQUIRE(Mtools(FEATHERDISK_MTYPE, image, "\"::Read Me First.txt\"") ==
            "Long names from mtools work.\nMore from FeatherDisk.\n");
    const std::string root = Mtools(FEATHERDISK_MDIR, image, "-b ::");
    REQUIRE(root.find("big.txt") == std::string::npos);
    REQUIRE(root.find("empty") == std::string::npos);
    REQUIRE(root.find("New Folder") != std::string::npos);
    const std::string docs = Mtools(FEATHERDISK_MDIR, image, "-b ::docs");
    REQUIRE(docs.find("notes.txt") == std::string::npos);
    REQUIRE(docs.find("My Plans.txt") != std::string::npos);
#endif
}

TEST_CASE("mtools reads a FAT16 volume written before and after MakeReadOnly and a remount")
{
#ifndef FEATHERDISK_MTOOLS_IMAGE
    SKIP("mtools is not installed");
#else
    std::unique_ptr<MemoryDevice> device = LoadImage();
    FeatherDisk::Filesystem* fat = Value(Fat16Type().Mount(*device, {}));
    WriteNewFile(*fat, "before.txt", "before\n");
    Succeeds(fat->MakeReadOnly());
    Succeeds(Fat16Type().Unmount(*fat));
    {
        MountedFilesystem again(Fat16Type(), *device);
        WriteNewFile(*again, "after.txt", "after\n");
    }
    const std::string image = std::string(FEATHERDISK_TEST_OUTPUT) + "/fat16-remount.img";
    SaveImage(*device, image);
    REQUIRE(Mtools(FEATHERDISK_MTYPE, image, "\"::before.txt\"") == "before\n");
    REQUIRE(Mtools(FEATHERDISK_MTYPE, image, "\"::after.txt\"") == "after\n");
#endif
}
