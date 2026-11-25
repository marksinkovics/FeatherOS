// SPDX-License-Identifier: MIT OR Apache-2.0
// FAT16 directory scans: slots after the first 0x00 are free whatever they hold, and walking a
// subdirectory reads each of its clusters' FAT entries once, not once per entry.
#include <catch2/catch_test_macros.hpp>

#include "common/little_endian.h"
#include "fs/fat.h"
#include "tests/fat_image.h"
#include "tests/filesystem_helpers.h"
#include "tests/memory_device.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace
{

using namespace FeatherDisk;
using namespace FeatherDisk::Tests;
using Fat::Fat16Type;

constexpr std::uint32_t EntriesPerCluster = 16;

// A directory entry for STALE.TXT, as another implementation may have left behind it.
void WriteStale(std::uint8_t* entry)
{
    std::memset(entry, 0, 32);
    std::memcpy(entry, "STALE   TXT", 11);
    entry[11] = 0x20;
    StoreLittleEndian<std::uint16_t>(entry + 26, 7);
    StoreLittleEndian<std::uint32_t>(entry + 28, 100);
}

bool Has(const std::vector<std::string>& names, const std::string& name)
{
    return std::find(names.begin(), names.end(), name) != names.end();
}

} // namespace

TEST_CASE("FAT16 one-slot create leaves stale entries after the end marker unreachable")
{
    Fat16Image image;
    {
        MountedFilesystem fat(Fat16Type(), image.Device());
        Value(fat->Create(fat->Root(), "A.TXT", 0644));
    }
    // [A.TXT][0x00][stale][0x00]...
    WriteStale(image.RootEntry(2));

    MountedFilesystem fat(Fat16Type(), image.Device());
    Value(fat->Create(fat->Root(), "B.TXT", 0644));
    const std::vector<std::string> names = List(*fat, fat->Root());
    CHECK(names == std::vector<std::string>{"A.TXT", "B.TXT"});
    CHECK(ErrorOf(fat->Lookup(fat->Root(), "STALE.TXT")) == Error::NotFound);
}

TEST_CASE("FAT16 long-name create after the end marker is found by readers")
{
    Fat16Image image;
    {
        MountedFilesystem fat(Fat16Type(), image.Device());
        Value(fat->Create(fat->Root(), "A.TXT", 0644));
    }
    // [A.TXT][0x00][stale][0x00]...: a run of three slots must start at the marker.
    WriteStale(image.RootEntry(2));

    MountedFilesystem fat(Fat16Type(), image.Device());
    const NodeId created = Value(fat->Create(fat->Root(), "a long file name.txt", 0644));
    CHECK(Value(fat->Lookup(fat->Root(), "a long file name.txt")) == created);
    const std::vector<std::string> names = List(*fat, fat->Root());
    CHECK(names == std::vector<std::string>{"A.TXT", "a long file name.txt"});
    CHECK_FALSE(Has(names, "STALE.TXT"));
}

TEST_CASE("FAT16 a run that starts at the end marker grows the subdirectory")
{
    Fat16Image image;
    MountedFilesystem fat(Fat16Type(), image.Device());
    const NodeId directory = Value(fat->MakeDirectory(fat->Root(), "d", 0755));
    // "." and ".." take two slots; leave only the last one free.
    for (std::uint32_t index = 0; index < EntriesPerCluster - 3; ++index)
    {
        Value(fat->Create(directory, "F" + std::to_string(index) + ".TXT", 0644));
    }
    const NodeId created = Value(fat->Create(directory, "a long file name.txt", 0644));
    CHECK(Value(fat->Lookup(directory, "a long file name.txt")) == created);
    CHECK(List(*fat, directory).size() == EntriesPerCluster - 3 + 1);
}

TEST_CASE("FAT16 scans a subdirectory in time linear in its clusters")
{
    constexpr std::uint16_t FirstDirectoryCluster = 3000;
    const auto reads_for_miss = [](std::uint32_t clusters)
    {
        Fat16Image image;
        std::uint32_t first = 0;
        {
            MountedFilesystem fat(Fat16Type(), image.Device());
            Value(fat->MakeDirectory(fat->Root(), "d", 0755));
            first = ScanFat16(image.Device()).Find("d")->first_cluster;
        }
        // Chain more clusters behind the directory and fill them with entries behind the driver's
        // back, so the setup does not itself scan the directory over and over.
        const std::uint32_t data_start = image.RootDirectorySector() + 1; // 16 root entries
        std::uint16_t previous = static_cast<std::uint16_t>(first);
        for (std::uint32_t index = 0; index < clusters - 1; ++index)
        {
            const std::uint16_t cluster = FirstDirectoryCluster + static_cast<std::uint16_t>(index);
            image.SetFatEntry(previous, cluster);
            image.SetFatEntry(cluster, 0xFFFF);
            previous = cluster;
            std::uint8_t* sector = image.Device().Sector(data_start + cluster - 2).data();
            for (std::uint32_t slot = 0; slot < EntriesPerCluster; ++slot)
            {
                std::uint8_t* entry = sector + slot * 32;
                WriteStale(entry);
                entry[1] = static_cast<std::uint8_t>('A' + index % 26);
                entry[2] = static_cast<std::uint8_t>('A' + slot);
            }
        }
        // The first cluster's free slots take entries too, so the directory has no end marker.
        std::uint8_t* sector = image.Device().Sector(data_start + first - 2).data();
        for (std::uint32_t slot = 2; slot < EntriesPerCluster; ++slot)
        {
            WriteStale(sector + slot * 32);
            sector[slot * 32 + 2] = static_cast<std::uint8_t>('A' + slot);
        }

        MountedFilesystem fat(Fat16Type(), image.Device());
        const NodeId directory = Value(fat->Lookup(fat->Root(), "d"));
        image.Device().read_count = 0;
        CHECK(ErrorOf(fat->Lookup(directory, "NOSUCH.TXT")) == Error::NotFound);
        return image.Device().read_count;
    };

    const std::uint64_t small = reads_for_miss(32);
    const std::uint64_t large = reads_for_miss(64);
    // One directory sector and one FAT entry per cluster, plus the odd re-read.
    CHECK(small <= 4 * 32);
    CHECK(large <= 4 * 64);
    CHECK(large < 3 * small);
}
